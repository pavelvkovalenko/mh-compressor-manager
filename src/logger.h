#pragma once
#include <string>
#include <syslog.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

enum class LogLevel { DEBUG, INFO, WARNING, ERROR };

class Logger {
public:
    static void init(const std::string& ident, bool debug);
    static void shutdown();
    static void log(LogLevel level, const std::string& message);
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warning(const std::string& msg);
    static void error(const std::string& msg);

    // printf-style методы для локализации (ТЗ §22.4)
    // __attribute__((format(printf, 1, 2)) — компилятор проверяет соответствие аргументов
    static void debug_fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static void info_fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static void warning_fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static void error_fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

private:
    static bool s_debug;
    static std::thread s_async_thread;
    static std::atomic<bool> s_running;
    static std::queue<std::pair<LogLevel, std::string>> s_log_queue;
    static std::mutex s_queue_mutex;
    static std::condition_variable s_queue_cv;
    static void async_writer();
};
