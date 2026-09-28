#include "Logging.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#include <share.h>
#endif

namespace
{
std::mutex gMutex;
std::FILE* gFile = nullptr;
LogLevel gMinimum = LogLevel::Info;

const char* levelName(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Debug:
            return "D";
        case LogLevel::Info:
            return "I";
        default:
            return "W";
    }
}
} // namespace

void logInit(const std::string& filePath, LogLevel minimum)
{
    std::lock_guard<std::mutex> lock(gMutex);
    gMinimum = minimum;
    // Truncated per start: the host is long-lived, and one run's log is what a bug report needs.
#ifdef _WIN32
    // fopen would read the UTF-8 path in the ANSI code page (a non-ASCII user name breaks it).
    const std::filesystem::path path(std::u8string(filePath.begin(), filePath.end()));
    if (std::FILE* f = _wfsopen(path.c_str(), L"w", _SH_DENYNO))
#else
    if (std::FILE* f = std::fopen(filePath.c_str(), "w"))
#endif
    {
        if (gFile)
            std::fclose(gFile);
        gFile = f;
    }
}

void logWrite(LogLevel level, const char* category, const std::string& text)
{
    if (level < gMinimum)
        return;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

    std::lock_guard<std::mutex> lock(gMutex);
    std::FILE* out = gFile ? gFile : stderr;
    std::fprintf(out, "%s %s [%s] %s\n", stamp, levelName(level), category, text.c_str());
    std::fflush(out);
}
