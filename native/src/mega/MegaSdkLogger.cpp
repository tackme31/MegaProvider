#include "MegaSdkLogger.h"

#include "app/Logging.h"

#include <cstring>

namespace
{
// SDK v10.17.0 ships a debug-only LOG_warn in computeSparseOffset64()
// (third_party/sdk/src/filefingerprint.cpp:169, inside #ifndef NDEBUG) that
// only echoes an always-false test hook, once per sparse-CRC block: 128 lines
// per fingerprint of any file over 8 KiB. Dropped here rather than patching
// the pinned submodule; harmless to lose, and Release builds never emit it.
bool isSdkLogNoise(const char* message)
{
    static constexpr char kSparseCrcNoise[] = "computeSparseOffset64:";
    return message && std::strncmp(message, kSparseCrcNoise, sizeof kSparseCrcNoise - 1) == 0;
}
} // namespace

void MegaSdkLogger::log(const char* time, int logLevel, const char* source, const char* message)
{
    if (isSdkLogNoise(message))
    {
        return;
    }

    // FATAL/ERROR/WARNING collapse onto one warning level: an SDK-internal FATAL
    // message aborting this app via qCritical/qFatal would be far more
    // surprising than losing that severity distinction.
    switch (logLevel)
    {
        case mega::MegaApi::LOG_LEVEL_FATAL:
        case mega::MegaApi::LOG_LEVEL_ERROR:
        case mega::MegaApi::LOG_LEVEL_WARNING:
            LOG_WARN("sdk") << time << source << message;
            break;
        case mega::MegaApi::LOG_LEVEL_INFO:
            LOG_INFO("sdk") << time << source << message;
            break;
        default: // DEBUG, VERBOSE
            LOG_DEBUG("sdk") << time << source << message;
            break;
    }
}
