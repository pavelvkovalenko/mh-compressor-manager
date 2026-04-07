#include "config.h"
#include "logger.h"
#include "compressor.h"
#include "monitor.h"
#include "threadpool.h"

#include <systemd/sd-daemon.h>
#include <csignal>
#include <atomic>
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif
#include <filesystem>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <memory>

namespace fs = std::filesystem;

// Глобальные переменные для обработки сигналов
std::atomic<bool> g_running{true};
std::unique_ptr<ThreadPool> g_pool;
std::unique_ptr<Monitor> g_monitor;
std::unique_ptr<Config> g_cfg;

// Метрики производительности
struct PerformanceMetrics {
    std::atomic<uint64_t> total_tasks{0};
    std::atomic<uint64_t> completed_tasks{0};
    std::atomic<uint64_t> failed_tasks{0};
    std::atomic<uint64_t> bytes_processed{0};
    std::atomic<uint64_t> bytes_compressed{0};
    std::chrono::steady_clock::time_point start_time;
    
    PerformanceMetrics() : start_time(std::chrono::steady_clock::now()) {}
    
    void log_summary() const {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        Logger::info("=== Performance Summary ===");
        Logger::info(std::format("Total tasks: {}", total_tasks.load()));
        Logger::info(std::format("Completed: {}", completed_tasks.load()));
        Logger::info(std::format("Failed: {}", failed_tasks.load()));
        Logger::info(std::format("Bytes processed: {}", bytes_processed.load()));
        Logger::info(std::format("Bytes compressed: {}", bytes_compressed.load()));
        if (bytes_processed.load() > 0) {
            double ratio = (1.0 - (double)bytes_compressed.load() / bytes_processed.load()) * 100;
            Logger::info(std::format("Compression ratio: {:.1f}%", ratio));
        }
        Logger::info(std::format("Duration: {} seconds", duration));
        if (duration > 0) {
            Logger::info(std::format("Tasks/sec: {:.2f}", (double)completed_tasks.load() / duration));
        }
    }
};

PerformanceMetrics g_metrics;

// Обработчик сигналов завершения
void signal_handler(int sig) {
    Logger::info(std::format("Received signal {}", sig));
    g_running = false;
    if (g_monitor) g_monitor->stop();
    if (g_pool) g_pool->stop();
}

// Проверка необходимости сжатия (идемпотентность)
bool should_compress(const fs::path& path, const Config& cfg) {
    if (!fs::exists(path)) {
        Logger::debug(std::format("File does not exist: {}", path.string()));
        return false;
    }

    std::string ext = path.extension().string();
    if (ext.size() > 0) ext = ext.substr(1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    // Быстрая проверка на сжатые форматы
    if (ext == "gz" || ext == "br") return false;
    
    // Проверяем расширение напрямую из конфигурации (O(n), где n - малое число расширений)
    bool ext_match = false;
    for (const auto& e : cfg.extensions) {
        if (e.size() == ext.size()) {
            std::string lower_e = e;
            std::transform(lower_e.begin(), lower_e.end(), lower_e.begin(), ::tolower);
            if (lower_e == ext) {
                ext_match = true;
                break;
            }
        }
    }
    
    if (!ext_match) {
        Logger::debug(std::format("Extension not in list: {}.{}", path.string(), ext));
        return false;
    }

    fs::path gz = path.string() + ".gz";
    fs::path br = path.string() + ".br";
    
    try {
        auto src_time = fs::last_write_time(path);
        bool gz_ok = fs::exists(gz) && fs::last_write_time(gz) >= src_time;
        bool br_ok = fs::exists(br) && fs::last_write_time(br) >= src_time;
        
        if (gz_ok && br_ok) {
            Logger::debug(std::format("File already compressed and up-to-date: {}", path.string()));
            return false;
        }
    } catch (const fs::filesystem_error& e) {
        Logger::warning(std::format("Error checking file times: {}", e.what()));
        return true;
    }

    return true;
}

// Определение приоритета задачи на основе размера файла
TaskPriority determine_priority(const fs::path& path) {
    try {
        auto size = fs::file_size(path);
        // Маленькие файлы (< 10KB) - высокий приоритет (быстрая обработка)
        // Средние файлы (10KB - 1MB) - нормальный приоритет
        // Большие файлы (> 1MB) - низкий приоритет (долгая обработка)
        if (size < 10240) {
            return TaskPriority::HIGH;
        } else if (size < 1048576) {
            return TaskPriority::NORMAL;
        } else {
            return TaskPriority::LOW;
        }
    } catch (...) {
        return TaskPriority::NORMAL;
    }
}

// Задача сжатия (выполняется в пуле потоков)
void compress_task(const fs::path& path) {
    if (!g_cfg) return;
    
    // I/O троттлинг - задержка между файлами
    if (g_cfg->io_delay_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(g_cfg->io_delay_us));
    }
    
    g_metrics.total_tasks++;
    auto start = std::chrono::steady_clock::now();
    
    Logger::info(std::format("Processing task for: {}", path.string()));

    if (!should_compress(path, *g_cfg)) {
        Logger::debug(std::format("Skipping compression (up-to-date or invalid): {}", path.string()));
        g_metrics.completed_tasks++;
        return;
    }

    uint64_t original_size = 0;
    uint64_t compressed_size = 0;
    
    try {
        original_size = fs::file_size(path);
    } catch (...) {
        Logger::warning(std::format("Cannot get file size: {}", path.string()));
    }

    if (!g_cfg->dry_run) {
        Logger::info(std::format("Compressing: {}", path.string()));
        
        bool gzip_success = false;
        bool brotli_success = false;

        // Приоритет brotli если включен - сначала сжимаем brotli (лучшее сжатие)
        bool prefer_brotli = (g_cfg->algorithms == "all" || g_cfg->algorithms == "brotli");
        
        if (prefer_brotli && (g_cfg->algorithms == "all" || g_cfg->algorithms == "brotli")) {
            brotli_success = Compressor::compress_brotli(path, path.string() + ".br", g_cfg->brotli_level);
            if (brotli_success && fs::exists(path.string() + ".br")) {
                Compressor::copy_metadata(path, path.string() + ".br");
                try {
                    compressed_size += fs::file_size(path.string() + ".br");
                } catch (...) {}
            }
        }
        
        if (g_cfg->algorithms == "all" || g_cfg->algorithms == "gzip") {
            gzip_success = Compressor::compress_gzip(path, path.string() + ".gz", g_cfg->gzip_level);
            if (gzip_success && fs::exists(path.string() + ".gz")) {
                Compressor::copy_metadata(path, path.string() + ".gz");
                try {
                    compressed_size += fs::file_size(path.string() + ".gz");
                } catch (...) {}
            }
        }
        
        // Если brotli не был выполнен первым (только gzip режим)
        if (!prefer_brotli && (g_cfg->algorithms == "all" || g_cfg->algorithms == "brotli")) {
            brotli_success = Compressor::compress_brotli(path, path.string() + ".br", g_cfg->brotli_level);
            if (brotli_success && fs::exists(path.string() + ".br")) {
                Compressor::copy_metadata(path, path.string() + ".br");
                try {
                    compressed_size += fs::file_size(path.string() + ".br");
                } catch (...) {}
            }
        }

        if (gzip_success || brotli_success) {
            Logger::info(std::format("Compression completed: {}", path.string()));
            g_metrics.completed_tasks++;
            g_metrics.bytes_processed += original_size;
            g_metrics.bytes_compressed += compressed_size;
        } else {
            Logger::error(std::format("Compression failed: {}", path.string()));
            g_metrics.failed_tasks++;
        }
    } else {
        Logger::info(std::format("[DRY RUN] Would compress: {}", path.string()));
        g_metrics.completed_tasks++;
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    Logger::debug(std::format("Task completed in {} ms", duration));
}

// Задача удаления сжатых копий
void delete_task(const fs::path& path) {
    if (!g_cfg) return;
    if (!g_cfg->dry_run) {
        Logger::info(std::format("Removing compressed copies for: {}", path.string()));
        // Используем безопасное удаление с проверками
        Compressor::safe_remove_compressed(path);
    } else {
        Logger::info(std::format("[DRY RUN] Would remove copies for: {}", path.string()));
    }
}

int main(int argc, char* argv[]) {
    // Загрузка конфигурации
    g_cfg = std::make_unique<Config>(load_config(argc, argv));
    
    // Инициализация логгера
    Logger::init("mh-compressor-manager", g_cfg->debug);
    Logger::info("Starting mh-compressor-manager");
    Logger::info(std::format("Target paths: {}", g_cfg->target_paths.size()));
    for (const auto& p : g_cfg->target_paths) {
        Logger::info(std::format("  - {}", p));
    }

    // Обработчики сигналов
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Настройка пула потоков с ограничением размера очереди и I/O
    int threads = g_cfg->threads;
    if (threads == 0) threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 2;
    constexpr size_t MAX_QUEUE_SIZE = 1000;  // Максимум задач в очереди
    size_t max_ios = g_cfg->max_active_ios > 0 ? g_cfg->max_active_ios : 0;
    Logger::info(std::format("Thread pool size: {}, max queue size: {}, max active I/O: {}", 
                             threads, MAX_QUEUE_SIZE, max_ios > 0 ? max_ios : SIZE_MAX));

    g_pool = std::make_unique<ThreadPool>(threads, MAX_QUEUE_SIZE, max_ios);

    // Инициализация монитора
    g_monitor = std::make_unique<Monitor>(*g_cfg);

    // Регистрация обработчиков событий с проверкой переполнения очереди и приоритетами
    g_monitor->set_task_handler([](const fs::path& p) {
        TaskPriority priority = determine_priority(p);
        if (!g_pool->enqueue([p]() { compress_task(p); }, priority)) {
            Logger::warning(std::format("Task queue full, skipping: {}", p.string()));
        }
    });
    g_monitor->set_delete_handler([](const fs::path& p) {
        // Задачи удаления имеют высокий приоритет
        if (!g_pool->enqueue([p]() { delete_task(p); }, TaskPriority::HIGH)) {
            Logger::warning(std::format("Delete task queue full, skipping: {}", p.string()));
        }
    });

    // Запуск мониторинга
    g_monitor->start();
    
    // Начальное сканирование существующих файлов
    g_monitor->scan_existing_files();

    // Уведомление systemd о готовности
    sd_notify(0, "READY=1");
    Logger::info("Service ready (sd_notify sent)");

    // Главный цикл с обработкой исключений
    try {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Exception in main loop: {}", e.what()));
        g_running = false;
    } catch (...) {
        Logger::error("Unknown exception in main loop");
        g_running = false;
    }

    // Вывод метрик производительности
    g_metrics.log_summary();

    Logger::info("Service stopped");
    
    // Корректное завершение работы с очисткой глобальных указателей
    g_monitor.reset();
    g_pool.reset();
    g_cfg.reset();
    
    // Корректное завершение работы логгера
    Logger::shutdown();
    
    return 0;
}