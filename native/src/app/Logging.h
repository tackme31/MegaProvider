#pragma once
#include <sstream>
#include <string>

// Qt-free stand-in for MegaExplorer's logging categories: the host is a plain console
// process. Lines go to the file given to logInit(), or stderr before that.
enum class LogLevel
{
    Debug,
    Info,
    Warning,
};

void logInit(const std::string& filePath, LogLevel minimum);
void logWrite(LogLevel level, const char* category, const std::string& text);

// Streams like qDebug(): values are separated by a space, and the line is written
// when the temporary dies at the end of the full expression.
class LogLine
{
public:
    LogLine(LogLevel level, const char* category) : mLevel(level), mCategory(category) {}
    ~LogLine() { logWrite(mLevel, mCategory, mText.str()); }
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    template <typename T> LogLine& operator<<(const T& value)
    {
        if (mText.tellp() > 0)
            mText << ' ';
        mText << value;
        return *this;
    }

    // An SDK source/message may legitimately be null.
    LogLine& operator<<(const char* value) { return *this << std::string(value ? value : "(null)"); }

private:
    LogLevel mLevel;
    const char* mCategory;
    std::ostringstream mText;
};

#define LOG_DEBUG(category) LogLine(LogLevel::Debug, category)
#define LOG_INFO(category) LogLine(LogLevel::Info, category)
#define LOG_WARN(category) LogLine(LogLevel::Warning, category)
