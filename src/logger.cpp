#include "logger.h"
#include "i18n.h"
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

// Максимальный размер очереди логов для предотвращения DoS через переполнение памяти
constexpr size_t MAX_LOG_QUEUE_SIZE = 10000;
// Максимальное время ожидания перед сбросом старых логов (мс)
constexpr int LOG_FLUSH_TIMEOUT_MS = 5000;

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
        // Ждем завершения потока логов с таймаутом для предотвращения зависания
        auto start = std::chrono::steady_clock::now();
        while (!s_log_queue.empty() &&
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start).count() < LOG_FLUSH_TIMEOUT_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Принудительный flush оставшихся логов перед закрытием
        if (!s_log_queue.empty()) {
            Logger::log(LogLevel::WARNING, _("Log queue still has messages after timeout, flushing remaining..."));
            // Даём ещё немного времени на обработку
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        s_async_thread.join();
    }
    closelog();
}

void Logger::log(LogLevel level, const std::string& message) {
    bool queue_full = false;
    std::pair<LogLevel, std::string> overflow_msg;
    
    {
        std::lock_guard<std::mutex> lock(s_queue_mutex);
        // Защита от переполнения очереди логов (DoS prevention)
        if (s_log_queue.size() >= MAX_LOG_QUEUE_SIZE) {
            queue_full = true;
            // Сбрасываем старые логи если очередь переполнена
            size_t drop_count = s_log_queue.size() / 4;  // Удаляем 25% старых записей
            for (size_t i = 0; i < drop_count && !s_log_queue.empty(); ++i) {
                s_log_queue.pop();
            }
            // Готовим предупреждение о переполнении для добавления после разблокировки
            if (message.find("Log queue overflow") == std::string::npos) {
                overflow_msg = {LogLevel::WARNING, "Log queue overflow - dropped old entries"};
            }
        }
        s_log_queue.emplace(level, message);
    }
    
    // Добавляем сообщение о переполнении вне блокировки чтобы избежать рекурсии
    if (queue_full && !overflow_msg.second.empty()) {
        std::lock_guard<std::mutex> lock(s_queue_mutex);
        s_log_queue.push(std::move(overflow_msg));
    }
    
    s_queue_cv.notify_one();
}

void Logger::debug(const std::string& msg) { if (s_debug) log(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg) { log(LogLevel::INFO, msg); }
void Logger::warning(const std::string& msg) { log(LogLevel::WARNING, msg); }
void Logger::error(const std::string& msg) { log(LogLevel::ERROR, msg); }

// ============================================================================
// printf-style перегрузки для локализации (ТЗ §22.4)
// Используют vsnprintf — совместимы с GCC 14/15, не требуют constexpr строк
// ============================================================================

void Logger::debug(const char* fmt, ...) {
    if (!s_debug) return;
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log(LogLevel::DEBUG, std::string(buf));
}

void Logger::info(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log(LogLevel::INFO, std::string(buf));
}

void Logger::warning(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log(LogLevel::WARNING, std::string(buf));
}

void Logger::error(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log(LogLevel::ERROR, std::string(buf));
}
