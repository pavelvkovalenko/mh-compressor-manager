#pragma once
#include <string>
#include <syslog.h>

enum class LogLevel { DEBUG, INFO, WARNING, ERROR };

class Logger {
public:
    static void init(const std::string& ident, bool debug);
    static void log(LogLevel level, const std::string& message);
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warning(const std::string& msg);
    static void error(const std::string& msg);
private:
    static bool s_debug;
};
