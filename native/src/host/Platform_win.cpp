#include "Platform.h"

#include "app/Logging.h"

#include <windows.h>
#include <sddl.h>

#include <chrono>
#include <thread>
#include <vector>

namespace
{

std::wstring toWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
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

class PipeConnection final : public Connection
{
public:
    explicit PipeConnection(HANDLE pipe) : mPipe(pipe) {}
    ~PipeConnection() override
    {
        FlushFileBuffers(mPipe);
        DisconnectNamedPipe(mPipe);
        CloseHandle(mPipe);
    }

    bool read(char* buffer, std::size_t size, std::size_t& received) override
    {
        DWORD n = 0;
        if (!ReadFile(mPipe, buffer, static_cast<DWORD>(size), &n, nullptr) || n == 0)
            return false;
        received = n;
        return true;
    }

    bool write(const char* data, std::size_t size) override
    {
        DWORD written = 0;
        return WriteFile(mPipe, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    }

private:
    HANDLE mPipe;
};

class PipeListener final : public Listener
{
public:
    PipeListener(std::wstring name, PSECURITY_DESCRIPTOR sd, HANDLE first)
        : mName(std::move(name)), mSd(sd), mPending(first)
    {
    }
    ~PipeListener() override
    {
        if (mPending != INVALID_HANDLE_VALUE)
            CloseHandle(mPending);
        LocalFree(mSd);
    }

    static HANDLE create(const std::wstring& name, PSECURITY_DESCRIPTOR sd, bool first)
    {
        SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
        return CreateNamedPipeW(name.c_str(),
                                PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
    }

    std::unique_ptr<Connection> accept() override
    {
        if (mPending == INVALID_HANDLE_VALUE)
        {
            mPending = create(mName, mSd, false);
            if (mPending == INVALID_HANDLE_VALUE)
            {
                LOG_WARN("host") << "CreateNamedPipe failed, error" << GetLastError();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                return nullptr;
            }
        }
        const HANDLE pipe = mPending;
        mPending = INVALID_HANDLE_VALUE;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(pipe);
            return nullptr;
        }
        return std::make_unique<PipeConnection>(pipe);
    }

private:
    std::wstring mName;
    PSECURITY_DESCRIPTOR mSd;
    HANDLE mPending; // the instance the next client will connect to
};

} // namespace

std::unique_ptr<Listener> Listener::open(const std::string& address)
{
    PSECURITY_DESCRIPTOR sd = ownerOnlySecurityDescriptor();
    if (!sd)
    {
        LOG_WARN("host") << "could not build the pipe's security descriptor; refusing to start";
        return nullptr;
    }
    const std::wstring name = L"\\\\.\\pipe\\" + toWide(address);
    const HANDLE first = PipeListener::create(name, sd, true);
    if (first == INVALID_HANDLE_VALUE)
    {
        // ERROR_ACCESS_DENIED here means another host already owns the name.
        LOG_INFO("host") << "cannot create pipe, error" << GetLastError() << "- another host is running?";
        LocalFree(sd);
        return nullptr;
    }
    return std::make_unique<PipeListener>(name, sd, first);
}

void detachFromStarter()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

long currentProcessId()
{
    return static_cast<long>(GetCurrentProcessId());
}
