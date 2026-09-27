#pragma once
#include <megaapi.h> // derives from mega::MegaLogger, a complete-type base class

// Bridges mega::MegaLogger's callback into the "sdk" log category, so SDK-internal
// diagnostics land in the same log file as our own. Lives in src/mega because
// deriving from mega::MegaLogger requires megaapi.h.
class MegaSdkLogger : public mega::MegaLogger
{
public:
    void log(const char* time, int logLevel, const char* source, const char* message) override;
};
