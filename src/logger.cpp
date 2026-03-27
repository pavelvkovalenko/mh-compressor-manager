#include "logger.h"
#include <iostream>
#include <format>
#include <cstdarg>

bool Logger::s_debug = false;

void Logger::init(const std::string& ident, bool debug) {
    s_debug = debug;
    openlog(ident.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
    if (debug) {
        setlogmask(LOG_UPTO(LOG_DEBUG));
    } else {
        setlogmask(LOG_UPTO(LOG_INFO));
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    int priority = LOG_INFO;
    switch (level) {
        case LogLevel::DEBUG: priority = LOG_DEBUG; break;
        case LogLevel::INFO: priority = LOG_INFO; break;
        case LogLevel::WARNING: priority = LOG_WARNING; break;
        case LogLevel::ERROR: priority = LOG_ERR; break;
    }
    syslog(priority, "%s", message.c_str());
    // Also output to stderr for journald capture if running manually
    std::cerr << "[" << priority << "] " << message << std::endl;
}

void Logger::debug(const std::string& msg) { if (s_debug) log(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg) { log(LogLevel::INFO, msg); }
void Logger::warning(const std::string& msg) { log(LogLevel::WARNING, msg); }
void Logger::error(const std::string& msg) { log(LogLevel::ERROR, msg); }
