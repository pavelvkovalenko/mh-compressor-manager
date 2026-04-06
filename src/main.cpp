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

namespace fs = std::filesystem;

// Глобальные переменные для обработки сигналов
std::atomic<bool> g_running{true};
ThreadPool* g_pool = nullptr;
Monitor* g_monitor = nullptr;
Config* g_cfg = nullptr;

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

// Задача сжатия (выполняется в пуле потоков)
void compress_task(const fs::path& path) {
    if (!g_cfg) return;
    
    Logger::info(std::format("Processing task for: {}", path.string()));

    if (!should_compress(path, *g_cfg)) {
        Logger::debug(std::format("Skipping compression (up-to-date or invalid): {}", path.string()));
        return;
    }

    if (!g_cfg->dry_run) {
        Logger::info(std::format("Compressing: {}", path.string()));
        
        bool gzip_success = false;
        bool brotli_success = false;

        if (g_cfg->algorithms == "all" || g_cfg->algorithms == "gzip") {
            gzip_success = Compressor::compress_gzip(path, path.string() + ".gz", g_cfg->gzip_level);
            if (gzip_success && fs::exists(path.string() + ".gz")) {
                Compressor::copy_metadata(path, path.string() + ".gz");
            }
        }
        
        if (g_cfg->algorithms == "all" || g_cfg->algorithms == "brotli") {
            brotli_success = Compressor::compress_brotli(path, path.string() + ".br", g_cfg->brotli_level);
            if (brotli_success && fs::exists(path.string() + ".br")) {
                Compressor::copy_metadata(path, path.string() + ".br");
            }
        }

        if (gzip_success || brotli_success) {
            Logger::info(std::format("Compression completed: {}", path.string()));
        } else {
            Logger::error(std::format("Compression failed: {}", path.string()));
        }
    } else {
        Logger::info(std::format("[DRY RUN] Would compress: {}", path.string()));
    }
}

// Задача удаления сжатых копий
void delete_task(const fs::path& path) {
    if (!g_cfg) return;
    if (!g_cfg->dry_run) {
        Logger::info(std::format("Removing compressed copies for: {}", path.string()));
        std::error_code ec;
        fs::remove(path.string() + ".gz", ec);
        fs::remove(path.string() + ".br", ec);
    } else {
        Logger::info(std::format("[DRY RUN] Would remove copies for: {}", path.string()));
    }
}

int main(int argc, char* argv[]) {
    // Загрузка конфигурации
    Config cfg = load_config(argc, argv);
    g_cfg = &cfg;
    
    // Инициализация логгера
    Logger::init("mh-compressor-manager", cfg.debug);
    Logger::info("Starting mh-compressor-manager");
    Logger::info(std::format("Target paths: {}", cfg.target_paths.size()));
    for (const auto& p : cfg.target_paths) {
        Logger::info(std::format("  - {}", p));
    }

    // Обработчики сигналов
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Настройка пула потоков с ограничением размера очереди
    int threads = cfg.threads;
    if (threads == 0) threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 2;
    constexpr size_t MAX_QUEUE_SIZE = 1000;  // Максимум задач в очереди
    Logger::info(std::format("Thread pool size: {}, max queue size: {}", threads, MAX_QUEUE_SIZE));

    ThreadPool pool(threads, MAX_QUEUE_SIZE);
    g_pool = &pool;

    // Инициализация монитора
    Monitor monitor(cfg);
    g_monitor = &monitor;

    // Регистрация обработчиков событий с проверкой переполнения очереди
    monitor.set_task_handler([&pool](const fs::path& p) {
        if (!pool.enqueue([p]() { compress_task(p); })) {
            Logger::warning(std::format("Task queue full, skipping: {}", p.string()));
        }
    });
    monitor.set_delete_handler([&pool](const fs::path& p) {
        if (!pool.enqueue([p]() { delete_task(p); })) {
            Logger::warning(std::format("Delete task queue full, skipping: {}", p.string()));
        }
    });

    // Запуск мониторинга
    monitor.start();
    
    // Начальное сканирование существующих файлов
    monitor.scan_existing_files();

    // Уведомление systemd о готовности
    sd_notify(0, "READY=1");
    Logger::info("Service ready (sd_notify sent)");

    // Главный цикл
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    Logger::info("Service stopped");
    return 0;
}