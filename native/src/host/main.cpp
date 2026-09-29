// megaprovider-host: owns one MegaApi and serves the PowerShell provider over a named pipe
// (Windows) or a Unix domain socket (elsewhere).
//
//   megaprovider-host --pipe <name or socket path> --data <dir> [--idle-minutes <n>] [--build-id <text>]
//
// Started on demand by the module. One process per pipe name: a second copy finds the
// pipe taken and exits. It exits by itself after --idle-minutes with no client connected.
// --build-id is echoed by `status`; a module that finds another build's host shuts it down.
// Protocol: docs/HOST.md.
#include "Platform.h"
#include "Service.h"
#include "app/Logging.h"
#include "mega/MegaSdkClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::atomic<int> gActiveConnections{0};
std::atomic<std::int64_t> gLastActivity{0}; // steady_clock seconds
std::mutex gStopMutex;
std::condition_variable gStopWake;
bool gStop = false;

std::int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void requestStop()
{
    {
        std::lock_guard<std::mutex> lock(gStopMutex);
        gStop = true;
    }
    gStopWake.notify_all();
}

// Paths go through char8_t: a plain std::string would be read in the ANSI code page on Windows.
std::filesystem::path pathFromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// One JSON document per line, UTF-8, over a Connection.
class LineChannel
{
public:
    explicit LineChannel(std::unique_ptr<Connection> connection) : mConnection(std::move(connection)) {}

    bool readLine(std::string& line)
    {
        for (;;)
        {
            const auto newline = mBuffer.find('\n');
            if (newline != std::string::npos)
            {
                line = mBuffer.substr(0, newline);
                mBuffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }
            char chunk[4096];
            std::size_t received = 0;
            if (!mConnection->read(chunk, sizeof chunk, received))
                return false;
            mBuffer.append(chunk, received);
        }
    }

    // Called from the request thread and, for progress, from the SDK's transfer thread.
    bool writeLine(const Json& message)
    {
        const std::string text = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
        std::lock_guard<std::mutex> lock(mWriteMutex);
        if (mBroken)
            return false;
        if (!mConnection->write(text.data(), text.size()))
            mBroken = true;
        return !mBroken;
    }

private:
    std::unique_ptr<Connection> mConnection;
    std::string mBuffer;
    std::mutex mWriteMutex;
    bool mBroken = false;
};

void serve(std::unique_ptr<Connection> connection, Service& service)
{
    ++gActiveConnections;
    {
        LineChannel channel(std::move(connection));
        std::string line;
        while (channel.readLine(line))
        {
            gLastActivity = nowSeconds();
            Json request;
            Json response;
            try
            {
                request = Json::parse(line);
            }
            catch (const Json::parse_error& e)
            {
                channel.writeLine({{"id", nullptr}, {"ok", false}, {"error", {{"code", 1}, {"message", e.what()}}}});
                continue;
            }
            const Json id = request.is_object() ? request.value("id", Json(nullptr)) : Json(nullptr);
            const Emit emit = [&](const Json& partial) {
                Json message = partial;
                message["id"] = id;
                return channel.writeLine(message);
            };
            response = service.handle(request, emit);
            response["id"] = id;
            channel.writeLine(response);
            if (service.stopRequested())
                requestStop();
        }
    }
    gLastActivity = nowSeconds();
    --gActiveConnections;
}

int run(const std::vector<std::string>& args)
{
    std::string pipeName;
    std::string dataDir;
    int idleMinutes = 60;
    std::string buildId;
    for (std::size_t i = 0; i + 1 < args.size(); i += 2)
    {
        if (args[i] == "--pipe")
            pipeName = args[i + 1];
        else if (args[i] == "--data")
            dataDir = args[i + 1];
        else if (args[i] == "--idle-minutes")
            idleMinutes = std::atoi(args[i + 1].c_str());
        else if (args[i] == "--build-id")
            buildId = args[i + 1];
    }
    if (pipeName.empty() || dataDir.empty())
    {
        std::fprintf(stderr,
                     "usage: megaprovider-host --pipe <name> --data <dir> [--idle-minutes <n>] [--build-id <text>]\n");
        return 2;
    }
    // First of all: a second host must leave without touching the first one's log (which
    // logInit truncates) or node cache. Until logInit, the log goes to stderr.
    std::unique_ptr<Listener> listener = Listener::open(pipeName);
    if (!listener)
        return 1;
    detachFromStarter();

    const std::filesystem::path data = pathFromUtf8(dataDir);
    std::filesystem::create_directories(data / "sdk");
    logInit(utf8(data / "host.log"), LogLevel::Info);

    // Absolute and fixed: the SDK keeps its node cache there, and missing it means
    // downloading the whole tree again on the next login. The SDK wants the trailing separator.
    MegaSdkClient client(utf8(data / "sdk") + static_cast<char>(std::filesystem::path::preferred_separator),
                         "MegaProvider/0.1");
    Service service(client, buildId);
    gLastActivity = nowSeconds();
    LOG_INFO("host") << "started, pid" << currentProcessId();

    std::thread([&] {
        for (;;)
            if (auto connection = listener->accept())
                std::thread(
                    [&service](std::unique_ptr<Connection> c) { serve(std::move(c), service); },
                    std::move(connection))
                    .detach();
    }).detach();

    {
        std::unique_lock<std::mutex> lock(gStopMutex);
        while (!gStop)
        {
            gStopWake.wait_for(lock, std::chrono::seconds(30));
            if (idleMinutes > 0 && gActiveConnections == 0 && nowSeconds() - gLastActivity > idleMinutes * 60)
            {
                LOG_INFO("host") << "idle for" << idleMinutes << "minutes, exiting";
                break;
            }
        }
    }

    LOG_INFO("host") << "stopping";
    client.shutdown();
    // Connection threads are detached and may be blocked in a read; don't wait for them.
    std::_Exit(0);
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(utf8(std::filesystem::path(argv[i])));
    return run(args);
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
