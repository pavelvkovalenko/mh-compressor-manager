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

bool Logger::s_debug = false;
std::thread Logger::s_async_thread;
std::atomic<bool> Logger::s_running{false};
std::queue<std::pair<LogLevel, std::string>> Logger::s_log_queue;
std::mutex Logger::s_queue_mutex;
std::condition_variable Logger::s_queue_cv;

void Logger::async_writer() {
    // Оптимизация: пакетная обработка логов для уменьшения блокировок
    constexpr size_t BATCH_SIZE = 32;
    std::vector<std::pair<LogLevel, std::string>> batch;
    batch.reserve(BATCH_SIZE);
    
    while (s_running || !s_log_queue.empty()) {
        std::unique_lock<std::mutex> lock(s_queue_mutex);
        s_queue_cv.wait_for(lock, std::chrono::milliseconds(100), [] {
            return !s_log_queue.empty() || !s_running;
        });
        
        // Забираем пакет сообщений за одну блокировку
        size_t count = 0;
        while (!s_log_queue.empty() && count < BATCH_SIZE) {
            batch.emplace_back(std::move(s_log_queue.front()));
            s_log_queue.pop();
            ++count;
        }
        
        lock.unlock();
        
        // Обрабатываем пакет вне блокировки
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
        s_async_thread.join();
    }
    closelog();
}

void Logger::log(LogLevel level, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(s_queue_mutex);
        s_log_queue.emplace(level, message);
    }
    s_queue_cv.notify_one();
}

void Logger::debug(const std::string& msg) { if (s_debug) log(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg) { log(LogLevel::INFO, msg); }
void Logger::warning(const std::string& msg) { log(LogLevel::WARNING, msg); }
void Logger::error(const std::string& msg) { log(LogLevel::ERROR, msg); }
