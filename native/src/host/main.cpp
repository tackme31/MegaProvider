// megaprovider-host: owns one MegaApi and serves the PowerShell provider over a named pipe.
//
//   megaprovider-host.exe --pipe <name> --data <dir> [--idle-minutes <n>]
//
// Started on demand by the module. One process per pipe name: a second copy finds the
// pipe taken and exits. It exits by itself after --idle-minutes with no client connected.
// Protocol: docs/HOST.md.
#include "Service.h"
#include "app/Logging.h"
#include "mega/MegaSdkClient.h"

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
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

std::string toUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The pipe carries the whole account (it can log in, read and delete), so only the
// user running the host may open it. The default pipe DACL would also let Everyone read.
PSECURITY_DESCRIPTOR ownerOnlySecurityDescriptor()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return nullptr;
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    std::vector<BYTE> buffer(length);
    PSECURITY_DESCRIPTOR sd = nullptr;
    LPWSTR sid = nullptr;
    if (GetTokenInformation(token, TokenUser, buffer.data(), length, &length)
        && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
    {
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
        LocalFree(sid);
    }
    CloseHandle(token);
    return sd;
}

class Connection
{
public:
    explicit Connection(HANDLE pipe) : mPipe(pipe) {}
    ~Connection()
    {
        FlushFileBuffers(mPipe);
        DisconnectNamedPipe(mPipe);
        CloseHandle(mPipe);
    }

    // One JSON document per line, UTF-8.
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
            DWORD read = 0;
            if (!ReadFile(mPipe, chunk, sizeof chunk, &read, nullptr) || read == 0)
                return false;
            mBuffer.append(chunk, read);
        }
    }

    // Called from the request thread and, for progress, from the SDK's transfer thread.
    bool writeLine(const Json& message)
    {
        const std::string text = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
        std::lock_guard<std::mutex> lock(mWriteMutex);
        if (mBroken)
            return false;
        DWORD written = 0;
        if (!WriteFile(mPipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
            || written != text.size())
            mBroken = true;
        return !mBroken;
    }

private:
    HANDLE mPipe;
    std::string mBuffer;
    std::mutex mWriteMutex;
    bool mBroken = false;
};

void serve(HANDLE pipe, Service& service)
{
    ++gActiveConnections;
    {
        Connection connection(pipe);
        std::string line;
        while (connection.readLine(line))
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
                connection.writeLine({{"id", nullptr}, {"ok", false}, {"error", {{"code", 1}, {"message", e.what()}}}});
                continue;
            }
            const Json id = request.is_object() ? request.value("id", Json(nullptr)) : Json(nullptr);
            const Emit emit = [&](const Json& partial) {
                Json message = partial;
                message["id"] = id;
                return connection.writeLine(message);
            };
            response = service.handle(request, emit);
            response["id"] = id;
            connection.writeLine(response);
            if (service.stopRequested())
                requestStop();
        }
    }
    gLastActivity = nowSeconds();
    --gActiveConnections;
}

void acceptLoop(const std::wstring& pipeName, Service& service, PSECURITY_DESCRIPTOR sd)
{
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    bool first = true;
    for (;;)
    {
        const HANDLE pipe = CreateNamedPipeW(pipeName.c_str(),
                                             PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                             PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (first)
            {
                // ERROR_ACCESS_DENIED here means another host already owns the name.
                LOG_INFO("host") << "cannot create pipe, error" << error << "- another host is running?";
                requestStop();
                return;
            }
            LOG_WARN("host") << "CreateNamedPipe failed, error" << error;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        first = false;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(pipe);
            continue;
        }
        std::thread([pipe, &service] { serve(pipe, service); }).detach();
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // No crash dialog: nobody is looking at this process.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    std::wstring pipeName;
    std::wstring dataDir;
    int idleMinutes = 60;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::wstring key = argv[i];
        if (key == L"--pipe")
            pipeName = L"\\\\.\\pipe\\" + std::wstring(argv[i + 1]);
        else if (key == L"--data")
            dataDir = argv[i + 1];
        else if (key == L"--idle-minutes")
            idleMinutes = _wtoi(argv[i + 1]);
    }
    if (pipeName.empty() || dataDir.empty())
    {
        std::fwprintf(stderr, L"usage: megaprovider-host --pipe <name> --data <dir> [--idle-minutes <n>]\n");
        return 2;
    }

    const std::filesystem::path data(dataDir);
    std::filesystem::create_directories(data / "sdk");
    logInit(toUtf8((data / "host.log").wstring()), LogLevel::Info);

    PSECURITY_DESCRIPTOR sd = ownerOnlySecurityDescriptor();
    if (!sd)
    {
        LOG_WARN("host") << "could not build the pipe's security descriptor; refusing to start";
        return 1;
    }

    // Absolute and fixed: the SDK keeps its node cache there, and missing it means
    // downloading the whole tree again on the next login.
    MegaSdkClient client(toUtf8((data / "sdk").wstring()) + "\\", "MegaProvider/0.1");
    Service service(client);
    gLastActivity = nowSeconds();
    LOG_INFO("host") << "started, pid" << GetCurrentProcessId();

    std::thread([&] { acceptLoop(pipeName, service, sd); }).detach();

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
    // Connection threads are detached and may be blocked in ReadFile; don't wait for them.
    ExitProcess(0);
}
