#include "config.h"
#include "logger.h"
#include "compressor.h"
#include "monitor.h"
#include "threadpool.h"
#include "security.h"

#include <systemd/sd-daemon.h>
#include <csignal>
#include <atomic>
#include <sys/signalfd.h>
#include <unistd.h>
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
#include <fcntl.h>
#include <sys/epoll.h>

namespace fs = std::filesystem;

// Глобальные переменные для обработки сигналов
std::atomic<bool> g_running{true};
std::atomic<bool> g_reload_config{false};
std::unique_ptr<ThreadPool> g_pool;
std::unique_ptr<Monitor> g_monitor;
std::unique_ptr<Config> g_cfg;
int g_signal_fd = -1;

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

// Обработчик сигналов завершения (минималистичный - только установка флага)
void signal_handler(int sig) {
    // Только устанавливаем флаг, вся обработка в event loop через signalfd
    if (sig == SIGTERM || sig == SIGINT) {
        g_running = false;
    } else if (sig == SIGHUP) {
        g_reload_config = true;
    }
}

// Инициализация signalfd для безопасной обработки сигналов (без race conditions)
bool init_signal_fd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    
    // Блокируем сигналы для стандартной обработки, чтобы они шли в signalfd
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        Logger::error("Failed to block signals");
        return false;
    }
    
    g_signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (g_signal_fd == -1) {
        Logger::error(std::format("Failed to create signalfd: {}", strerror(errno)));
        return false;
    }
    
    Logger::info("Signal FD initialized for safe signal handling");
    return true;
}

// Обработка сигналов через signalfd (безопасно, без race conditions)
void handle_signals() {
    struct signalfd_siginfo fdsi;
    ssize_t s = read(g_signal_fd, &fdsi, sizeof(fdsi));
    if (s != sizeof(fdsi)) {
        return;
    }
    
    switch (fdsi.ssi_signo) {
        case SIGTERM:
        case SIGINT:
            Logger::info(std::format("Received signal {} ({}), initiating graceful shutdown", 
                                     fdsi.ssi_signo, strsignal(fdsi.ssi_signo)));
            g_running = false;
            if (g_monitor) {
                Logger::info("Stopping monitor...");
                g_monitor->stop();
            }
            if (g_pool) {
                Logger::info("Stopping thread pool (waiting for active tasks)...");
                g_pool->stop();  // ThreadPool ждет завершения активных задач
            }
            break;
        case SIGHUP:
            Logger::info("Received SIGHUP, scheduling config reload...");
            g_reload_config = true;
            break;
        default:
            Logger::warning(std::format("Received unexpected signal: {}", fdsi.ssi_signo));
            break;
    }
}

// Горячая перезагрузка конфигурации (SIGHUP handler)
void reload_config() {
    Logger::info("Reloading configuration...");
    try {
        auto old_cfg = std::move(g_cfg);
        g_cfg = std::make_unique<Config>(load_config(0, nullptr));
        
        Logger::info("Configuration reloaded successfully");
        Logger::info(std::format("New target paths: {}", g_cfg->target_paths.size()));
        
        if (g_monitor) {
            g_monitor->stop();
            g_monitor = std::make_unique<Monitor>(*g_cfg);
            
            g_monitor->set_task_handler([](const fs::path& p) {
                TaskPriority priority = determine_priority(p);
                if (!g_pool->enqueue([p]() { compress_task(p); }, priority)) {
                    Logger::warning(std::format("Task queue full, skipping: {}", p.string()));
                }
            });
            g_monitor->set_delete_handler([](const fs::path& p) {
                if (!g_pool->enqueue([p]() { delete_task(p); }, TaskPriority::HIGH)) {
                    Logger::warning(std::format("Delete task queue full, skipping: {}", p.string()));
                }
            });
            
            g_monitor->start();
            g_monitor->scan_existing_files();
        }
        
        Logger::info("Monitor restarted with new configuration");
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to reload configuration: {}", e.what()));
        Logger::warning("Keeping old configuration");
        if (old_cfg) {
            g_cfg = std::move(old_cfg);
        }
    }
}

// Проверка необходимости сжатия (идемпотентность)
bool should_compress(const fs::path& path, const Config& cfg) {
    // === КРИТИЧЕСКАЯ БЕЗОПАСНОСТЬ: Используем lstat вместо fs.exists для предотвращения symlink атак ===
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        Logger::debug(std::format("File does not exist or inaccessible: {}", path.string()));
        return false;
    }
    
    // Проверка: файл не должен быть symlink - это потенциальная атака
    if (S_ISLNK(st.st_mode)) {
        Logger::error(std::format("SECURITY: Path is a symlink (potential attack): {}", path.string()));
        return false;
    }
    
    // Проверка: файл должен быть обычным файлом
    if (!S_ISREG(st.st_mode)) {
        Logger::debug(std::format("Path is not a regular file: {}", path.string()));
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
        
        // Безопасная проверка сжатых файлов через lstat (не следует за symlink)
        struct stat gz_st, br_st;
        bool gz_exists = (lstat(gz.c_str(), &gz_st) == 0 && !S_ISLNK(gz_st.st_mode) && S_ISREG(gz_st.st_mode));
        bool br_exists = (lstat(br.c_str(), &br_st) == 0 && !S_ISLNK(br_st.st_mode) && S_ISREG(br_st.st_mode));
        
        bool gz_ok = false, br_ok = false;
        if (gz_exists) {
            gz_ok = fs::last_write_time(gz) >= src_time;
        }
        if (br_exists) {
            br_ok = fs::last_write_time(br) >= src_time;
        }
        
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
    } catch (const fs::filesystem_error& e) {
        Logger::warning(std::format("Filesystem error getting file size: {} - {}", path.string(), e.what()));
        return TaskPriority::NORMAL;
    } catch (const std::exception& e) {
        Logger::warning(std::format("Error getting file size: {} - {}", path.string(), e.what()));
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
    } catch (const fs::filesystem_error& e) {
        Logger::warning(std::format("Cannot get file size: {} - {}", path.string(), e.what()));
    } catch (const std::exception& e) {
        Logger::warning(std::format("Error getting file size: {} - {}", path.string(), e.what()));
    }

    if (!g_cfg->dry_run) {
        Logger::info(std::format("Compressing: {}", path.string()));
        
        bool gzip_success = false;
        bool brotli_success = false;

        // Приоритет brotli если включен - сначала сжимаем brotli (лучшее сжатие)
        bool prefer_brotli = (g_cfg->algorithms == "all" || g_cfg->algorithms == "brotli");
        
        if (prefer_brotli && (g_cfg->algorithms == "all" || g_cfg->algorithms == "brotli")) {
            brotli_success = Compressor::compress_brotli(path, path.string() + ".br", g_cfg->brotli_level);
            if (brotli_success) {
                // Безопасная проверка существования файла через lstat после сжатия
                struct stat br_st;
                if (lstat((path.string() + ".br").c_str(), &br_st) == 0 && S_ISREG(br_st.st_mode)) {
                    Compressor::copy_metadata(path, path.string() + ".br");
                    try {
                        compressed_size += fs::file_size(path.string() + ".br");
                    } catch (const fs::filesystem_error& e) {
                        Logger::warning(std::format("Cannot get compressed file size: {} - {}", path.string() + ".br", e.what()));
                    } catch (const std::exception& e) {
                        Logger::warning(std::format("Error getting compressed file size: {} - {}", path.string() + ".br", e.what()));
                    }
                }
            }
        }
        
        if (g_cfg->algorithms == "all" || g_cfg->algorithms == "gzip") {
            gzip_success = Compressor::compress_gzip(path, path.string() + ".gz", g_cfg->gzip_level);
            if (gzip_success) {
                // Безопасная проверка существования файла через lstat после сжатия
                struct stat gz_st;
                if (lstat((path.string() + ".gz").c_str(), &gz_st) == 0 && S_ISREG(gz_st.st_mode)) {
                    Compressor::copy_metadata(path, path.string() + ".gz");
                    try {
                        compressed_size += fs::file_size(path.string() + ".gz");
                    } catch (const fs::filesystem_error& e) {
                        Logger::warning(std::format("Cannot get compressed file size: {} - {}", path.string() + ".gz", e.what()));
                    } catch (const std::exception& e) {
                        Logger::warning(std::format("Error getting compressed file size: {} - {}", path.string() + ".gz", e.what()));
                    }
                }
            }
        }
        
        // Если brotli не был выполнен первым (только gzip режим)
        if (!prefer_brotli && (g_cfg->algorithms == "all" || g_cfg->algorithms == "brotli")) {
            brotli_success = Compressor::compress_brotli(path, path.string() + ".br", g_cfg->brotli_level);
            if (brotli_success) {
                // Безопасная проверка существования файла через lstat после сжатия
                struct stat br_st;
                if (lstat((path.string() + ".br").c_str(), &br_st) == 0 && S_ISREG(br_st.st_mode)) {
                    Compressor::copy_metadata(path, path.string() + ".br");
                    try {
                        compressed_size += fs::file_size(path.string() + ".br");
                    } catch (const fs::filesystem_error& e) {
                        Logger::warning(std::format("Cannot get compressed file size: {} - {}", path.string() + ".br", e.what()));
                    } catch (const std::exception& e) {
                        Logger::warning(std::format("Error getting compressed file size: {} - {}", path.string() + ".br", e.what()));
                    }
                }
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
    
    // Инициализация логгера ДО сброса привилегий (чтобы логи писались от root)
    Logger::init("mh-compressor-manager", g_cfg->debug);
    Logger::info("Starting mh-compressor-manager");
    Logger::info(std::format("Target paths: {}", g_cfg->target_paths.size()));
    for (const auto& p : g_cfg->target_paths) {
        Logger::info(std::format("  - {}", p));
    }

    // Сброс привилегий и инициализация песочницы (только если запущены от root)
    if (security::is_running_as_root()) {
        if (g_cfg->drop_privileges) {
            Logger::info("Running as root, attempting to drop privileges...");
            if (!security::drop_privileges(g_cfg->run_as_user, g_cfg->target_paths)) {
                Logger::error("Failed to drop privileges, exiting for safety");
                return 1;
            }
            Logger::info("Privileges dropped successfully");
        } else {
            Logger::warning("Running as root with drop_privileges=false (not recommended)");
        }
        
        // Инициализация seccomp после сброса прав и всех инициализирующих вызовов
        if (g_cfg->enable_seccomp) {
            Logger::info("Initializing seccomp sandbox...");
            if (!security::init_seccomp()) {
                Logger::warning("Failed to initialize seccomp, continuing without sandbox");
            } else {
                Logger::info("Seccomp sandbox active");
            }
        }
    } else {
        Logger::debug("Not running as root, skipping privilege drop and seccomp");
    }

    // Инициализация безопасной обработки сигналов через signalfd
    if (!init_signal_fd()) {
        Logger::error("Failed to initialize signal handling, falling back to basic signals");
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGHUP, signal_handler);
    }

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

    // Главный цикл с использованием epoll для обработки сигналов через signalfd
    try {
        while (g_running) {
            // Проверяем необходимость перезагрузки конфигурации
            if (g_reload_config) {
                g_reload_config = false;
                reload_config();
            }
            
            // Если signalfd инициализирован, используем epoll для ожидания сигналов
            if (g_signal_fd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = g_signal_fd;
                
                int epfd = epoll_create1(EPOLL_CLOEXEC);
                if (epfd >= 0) {
                    epoll_ctl(epfd, EPOLL_CTL_ADD, g_signal_fd, &ev);
                    
                    struct epoll_event events[1];
                    int nfds = epoll_wait(epfd, events, 1, 1000);  // Таймаут 1 секунда
                    
                    if (nfds > 0 && events[0].events & EPOLLIN) {
                        handle_signals();
                    }
                    
                    close(epfd);
                } else {
                    // Fallback если epoll не работает
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            } else {
                // Fallback для старой обработки сигналов
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Exception in main loop: {}", e.what()));
        g_running = false;
    }

    // Вывод метрик производительности
    g_metrics.log_summary();

    Logger::info("Service stopped gracefully");
    
    // Корректное завершение работы с очисткой глобальных указателей
    g_monitor.reset();
    g_pool.reset();
    g_cfg.reset();
    
    // Закрываем signalfd если был открыт
    if (g_signal_fd >= 0) {
        close(g_signal_fd);
        g_signal_fd = -1;
    }
    
    // Корректное завершение работы логгера
    Logger::shutdown();
    
    return 0;
}