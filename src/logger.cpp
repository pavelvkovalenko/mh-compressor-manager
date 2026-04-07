#include "logger.h"
#include <iostream>
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif
#include <cstdarg>
#include <chrono>

constexpr size_t MAX_LOG_QUEUE_SIZE = 10000;
constexpr int LOG_FLUSH_TIMEOUT_MS = 5000;

bool Logger::s_debug = false;
std::thread Logger::s_async_thread;
std::atomic<bool> Logger::s_running{false};
std::queue<std::pair<LogLevel, std::string>> Logger::s_log_queue;
std::mutex Logger::s_queue_mutex;
std::condition_variable Logger::s_queue_cv;
std::unordered_map<std::string, LogRateLimit> Logger::s_rate_limits;
std::mutex Logger::s_rate_limit_mutex;

bool Logger::should_rate_limit(const std::string& message) {
    std::string base_message = message;
    for (char& c : base_message) {
        if (c >= '0' && c <= '9') c = '#';
    }
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(s_rate_limit_mutex);
    auto& limit = s_rate_limits[base_message];
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - limit.last_log).count();
    if (elapsed > LogRateLimit::INTERVAL_MS) {
        limit.count = 0;
        limit.last_log = now;
    }
    limit.count++;
    if (limit.count > LogRateLimit::MAX_LOGS_PER_INTERVAL) {
        return true;
    }
    return false;
}

void Logger::async_writer() {
    constexpr size_t BATCH_SIZE = 32;
    std::vector<std::pair<LogLevel, std::string>> batch;
    batch.reserve(BATCH_SIZE);
    while (s_running || !s_log_queue.empty()) {
        std::unique_lock<std::mutex> lock(s_queue_mutex);
        s_queue_cv.wait_for(lock, std::chrono::milliseconds(100), [] {
            return !s_log_queue.empty() || !s_running;
        });
        size_t count = 0;
        while (!s_log_queue.empty() && count < BATCH_SIZE) {
            batch.emplace_back(std::move(s_log_queue.front()));
            s_log_queue.pop();
            ++count;
        }
        lock.unlock();
        for (auto& [level, message] : batch) {
            int priority = LOG_INFO;
            switch (level) {
                case LogLevel::DEBUG: priority = LOG_DEBUG; break;
                case LogLevel::INFO: priority = LOG_INFO; break;
                case LogLevel::WARNING: priority = LOG_WARNING; break;
                case LogLevel::ERROR: priority = LOG_ERR; break;
            }
            syslog(priority, "%s", message.c_str());
            if (s_debug) {
                std::cerr << "[" << priority << "] " << message << std::endl;
            }
        }
        batch.clear();
    }
}

void Logger::init(const std::string& ident, bool debug) {
    s_debug = debug;
    openlog(ident.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
    if (debug) {
        setlogmask(LOG_UPTO(LOG_DEBUG));
    } else {
        setlogmask(LOG_UPTO(LOG_INFO));
    }
    s_running = true;
    s_async_thread = std::thread(async_writer);
}

void Logger::shutdown() {
    s_running = false;
    s_queue_cv.notify_all();
    if (s_async_thread.joinable()) {
        auto start = std::chrono::steady_clock::now();
        while (!s_log_queue.empty() && 
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start).count() < LOG_FLUSH_TIMEOUT_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        s_async_thread.join();
    }
    closelog();
}

void Logger::log(LogLevel level, const std::string& message) {
    if ((level == LogLevel::WARNING || level == LogLevel::ERROR) && should_rate_limit(message)) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_queue_mutex);
    if (s_log_queue.size() >= MAX_LOG_QUEUE_SIZE) {
        size_t drop_count = s_log_queue.size() / 4;
        for (size_t i = 0; i < drop_count && !s_log_queue.empty(); ++i) {
            s_log_queue.pop();
        }
        if (message.find("Log queue overflow") == std::string::npos) {
            s_log_queue.emplace(LogLevel::WARNING, "Log queue overflow - dropped old entries");
        }
    }
    s_log_queue.emplace(level, message);
    s_queue_cv.notify_one();
}

void Logger::debug(const std::string& msg) { if (s_debug) log(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg) { log(LogLevel::INFO, msg); }
void Logger::warning(const std::string& msg) { log(LogLevel::WARNING, msg); }
void Logger::error(const std::string& msg) { log(LogLevel::ERROR, msg); }
