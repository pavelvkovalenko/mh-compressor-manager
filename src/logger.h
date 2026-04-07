#pragma once
#include <string>
#include <syslog.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <chrono>
#include <unordered_map>

enum class LogLevel { DEBUG, INFO, WARNING, ERROR };

// Структура для rate limiting логов
struct LogRateLimit {
    std::chrono::steady_clock::time_point last_log;
    int count;
    static constexpr int MAX_LOGS_PER_INTERVAL = 5;
    static constexpr int INTERVAL_MS = 1000;
};

class Logger {
public:
    static void init(const std::string& ident, bool debug);
    static void shutdown();
    static void log(LogLevel level, const std::string& message);
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warning(const std::string& msg);
    static void error(const std::string& msg);
    
private:
    static bool s_debug;
    static std::thread s_async_thread;
    static std::atomic<bool> s_running;
    static std::queue<std::pair<LogLevel, std::string>> s_log_queue;
    static std::mutex s_queue_mutex;
    static std::condition_variable s_queue_cv;
    static std::unordered_map<std::string, LogRateLimit> s_rate_limits;
    static std::mutex s_rate_limit_mutex;
    static void async_writer();
    static bool should_rate_limit(const std::string& message);
};
