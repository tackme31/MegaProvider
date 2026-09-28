#include "Platform.h"

#include "app/Logging.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{

std::string lastError()
{
    return std::strerror(errno);
}

class SocketConnection final : public Connection
{
public:
    explicit SocketConnection(int fd) : mFd(fd) {}
    ~SocketConnection() override
    {
        ::shutdown(mFd, SHUT_RDWR);
        ::close(mFd);
    }

    bool read(char* buffer, std::size_t size, std::size_t& received) override
    {
        for (;;)
        {
            const ssize_t n = ::recv(mFd, buffer, size, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            received = static_cast<std::size_t>(n);
            return true;
        }
    }

    bool write(const char* data, std::size_t size) override
    {
        while (size > 0)
        {
            // MSG_NOSIGNAL: a client that went away must fail the write (which cancels its
            // transfer), not raise SIGPIPE and take the whole host down.
            const ssize_t n = ::send(mFd, data, size, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

private:
    int mFd;
};

class SocketListener final : public Listener
{
public:
    explicit SocketListener(int fd) : mFd(fd) {}
    ~SocketListener() override { ::close(mFd); }

    std::unique_ptr<Connection> accept() override
    {
        const int fd = ::accept4(mFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
        {
            if (errno != EINTR)
                LOG_WARN("host") << "accept failed:" << lastError();
            return nullptr;
        }
        return std::make_unique<SocketConnection>(fd);
    }

private:
    int mFd;
};

bool tryBind(int fd, const sockaddr_un& address)
{
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
}

// A socket file with nobody behind it is what a host leaves when it exits; one that answers
// belongs to a running host.
bool someoneListens(const sockaddr_un& address)
{
    const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0)
        return false;
    const bool answered = ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
    ::close(probe);
    return answered;
}

} // namespace

std::unique_ptr<Listener> Listener::open(const std::string& address)
{
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (address.empty() || address.size() >= sizeof addr.sun_path)
    {
        LOG_WARN("host") << "socket path is empty or too long:" << address;
        return nullptr;
    }
    std::memcpy(addr.sun_path, address.c_str(), address.size() + 1);

    // The socket carries the whole account, so its folder admits only this user. Checked on the
    // folder, not just the socket: connecting needs search permission on every directory above.
    const std::filesystem::path dir = std::filesystem::path(address).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (::chmod(dir.c_str(), S_IRWXU) != 0)
    {
        LOG_WARN("host") << "cannot restrict" << dir.string() << ":" << lastError();
        return nullptr;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        LOG_WARN("host") << "socket failed:" << lastError();
        return nullptr;
    }
    if (!tryBind(fd, addr))
    {
        if (errno != EADDRINUSE || someoneListens(addr))
        {
            LOG_INFO("host") << "cannot bind" << address << ":" << lastError() << "- another host is running?";
            ::close(fd);
            return nullptr;
        }
        ::unlink(address.c_str());
        if (!tryBind(fd, addr))
        {
            LOG_WARN("host") << "cannot bind" << address << "after removing a stale socket:" << lastError();
            ::close(fd);
            return nullptr;
        }
    }
    ::chmod(address.c_str(), S_IRUSR | S_IWUSR);
    if (::listen(fd, 16) != 0)
    {
        LOG_WARN("host") << "listen failed:" << lastError();
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<SocketListener>(fd);
}

void detachFromStarter()
{
    // Its own session: closing the starter's terminal (SIGHUP) or Ctrl+C there (SIGINT to the
    // foreground group) must not reach it.
    ::setsid();
    // Let go of the starter's stdio at once. Holding an inherited pipe would keep
    // `pwsh -File x.ps1 | sed` from ending until the host exits (docs/HOST.md).
    const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null >= 0)
    {
        ::dup2(null, STDIN_FILENO);
        ::dup2(null, STDOUT_FILENO);
        ::dup2(null, STDERR_FILENO);
        if (null > STDERR_FILENO)
            ::close(null);
    }
    std::signal(SIGPIPE, SIG_IGN);
}

long currentProcessId()
{
    return static_cast<long>(::getpid());
}
