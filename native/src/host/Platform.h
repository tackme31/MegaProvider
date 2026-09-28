#pragma once
// What differs between Windows and Unix: how clients reach the host (a named pipe or a
// Unix domain socket) and how it cuts itself loose from the process that started it.
// Platform_win.cpp / Platform_unix.cpp; the protocol above this is shared (main.cpp).

#include <cstddef>
#include <memory>
#include <string>

// One connected client. Blocking; read from its own thread, written from that thread and,
// for progress, from the SDK's transfer thread (the caller serializes writes).
class Connection
{
public:
    virtual ~Connection() = default;
    // False at end of stream or on an error.
    virtual bool read(char* buffer, std::size_t size, std::size_t& received) = 0;
    // False once the client has gone away.
    virtual bool write(const char* data, std::size_t size) = 0;
};

class Listener
{
public:
    // `address` is the pipe name on Windows and the socket path on Unix (UTF-8). Only the user
    // running the host can connect. Null when another host already owns the address, or it
    // could not be set up; the reason is logged.
    static std::unique_ptr<Listener> open(const std::string& address);

    virtual ~Listener() = default;
    // Blocks until a client connects. Null on a transient failure (logged; call again).
    virtual std::unique_ptr<Connection> accept() = 0;
};

// Called once, after the command line is parsed: nobody watches this process, so no crash
// dialogs (Windows) and no hold on the starter's terminal or output (Unix).
void detachFromStarter();

long currentProcessId();
