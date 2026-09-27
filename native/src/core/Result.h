#pragma once
#include <cassert>
#include <string>
#include <utility>

// `code` is a MegaErrorCodes.h value and is mandatory: callers branch on errorCode,
// never on errorMessage, which is a fixed English SDK string.
//
// [[nodiscard]] sits on the class, so discarding *any* Result-returning call warns.
// Ignoring a failure on purpose is written `(void)foo();` with a reason.
template<typename T>
struct [[nodiscard]] Result
{
    bool success = false;
    std::string errorMessage;
    int errorCode = 0;

    // A failed Result's value is a default-constructed T -- an empty string or 0 that
    // looks like real data. The assert turns that silent misread into a Debug crash.
    T& value()
    {
        assert(success && "read value() of a failed Result");
        return mValue;
    }
    const T& value() const
    {
        assert(success && "read value() of a failed Result");
        return mValue;
    }

    static Result<T> ok(T v)
    {
        Result<T> r;
        r.success = true;
        r.mValue = std::move(v);
        return r;
    }
    static Result<T> fail(std::string message, int code)
    {
        Result<T> r;
        r.success = false;
        r.errorMessage = std::move(message);
        r.errorCode = code;
        return r;
    }

private:
    T mValue{};
};

// void has no value member, so this needs its own specialization.
template<>
struct [[nodiscard]] Result<void>
{
    bool success = false;
    std::string errorMessage;
    int errorCode = 0;

    static Result<void> ok()
    {
        Result<void> r;
        r.success = true;
        return r;
    }
    static Result<void> fail(std::string message, int code)
    {
        Result<void> r;
        r.success = false;
        r.errorMessage = std::move(message);
        r.errorCode = code;
        return r;
    }
};
