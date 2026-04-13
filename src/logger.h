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

    // Перегрузка 1: готовая строка (без форматирования)
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warning(const std::string& msg);
    static void error(const std::string& msg);

    // Перегрузка 2: printf-style форматирование (для локализации)
    // __attribute__((format(printf, 1, 2)) — компилятор проверяет соответствие аргументов
    static void debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static void warning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static void error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

private:
    static bool s_debug;
    static std::thread s_async_thread;
    static std::atomic<bool> s_running;
    static std::queue<std::pair<LogLevel, std::string>> s_log_queue;
    static std::mutex s_queue_mutex;
    static std::condition_variable s_queue_cv;
    static void async_writer();
};
