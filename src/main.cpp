#include "config.h"
#include "logger.h"
#include "compressor.h"
#include "monitor.h"
#include "threadpool.h"
#include "security.h"
#include "async_io.h"
#include "memory_pool.h"
#include "cache_info.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
#include <sys/signalfd.h>
#include <cstring>
#include <cstdio>
#include <sys/epoll.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <memory>
#include <future>
#include <shared_mutex>
#include <systemd/sd-daemon.h>
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif

namespace fs = std::filesystem;

// ============================================================================
// КОНСТАНТЫ GRACEFUL SHUTDOWN
// ============================================================================
constexpr std::chrono::seconds SHUTDOWN_TIMEOUT{30};  // Максимальное время на завершение задач
constexpr std::chrono::milliseconds SHUTDOWN_CHECK_INTERVAL{100};  // Интервал проверки статуса

// Forward declarations для функций которые используются в lambda
TaskPriority determine_priority(const fs::path& path);
void compress_task(const fs::path& path);
void delete_task(const fs::path& path);

// Глобальные переменные для обработки сигналов
std::atomic<bool> g_running{true};
std::atomic<bool> g_reload_config{false};  // Атомарная переменная для защиты от race condition
std::unique_ptr<ThreadPool> g_pool;
std::unique_ptr<Monitor> g_monitor;
std::unique_ptr<Config> g_cfg;
std::shared_mutex g_cfg_mutex;  // Мьютекс для защиты g_cfg от одновременного доступа
int g_signal_fd = -1;
// Начальное сканирование деревьев вынесено из критического пути systemd READY=1
std::thread g_initial_scan_thread;

// Метрики производительности
struct PerformanceMetrics {
    std::atomic<uint64_t> total_tasks{0};
    std::atomic<uint64_t> completed_tasks{0};
    std::atomic<uint64_t> failed_tasks{0};
    std::atomic<uint64_t> bytes_processed{0};
    std::atomic<uint64_t> bytes_compressed{0};
    // Метрики пропущенных малых файлов (ТЗ §4)
    std::atomic<uint64_t> files_skipped_small{0};
    std::atomic<uint64_t> bytes_skipped_small{0};
    std::chrono::steady_clock::time_point start_time;

    PerformanceMetrics() : start_time(std::chrono::steady_clock::now()) {}

    void log_summary() const {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

        Logger::info("=== Performance Summary ===");
        Logger::info(std::format("Total tasks: {}", total_tasks.load()));
        Logger::info(std::format("Completed: {}", completed_tasks.load()));
        Logger::info(std::format("Failed: {}", failed_tasks.load()));
        Logger::info(std::format("Skipped (too small): {}", files_skipped_small.load()));
        Logger::info(std::format("Bytes skipped (small): {}", bytes_skipped_small.load()));
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
CacheInfo g_cache;  // Кэш CPU (определяется при старте, ТЗ §3.2.9)

// Безопасное получение копии указателя на конфигурацию
std::shared_ptr<Config> get_config() {
    std::shared_lock<std::shared_mutex> lock(g_cfg_mutex);
    if (!g_cfg) return nullptr;
    return std::make_shared<Config>(*g_cfg);
}

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
            // НЕ останавливаем монитор здесь — graceful_shutdown_with_timeout() сделает это
            // Только сигнализируем о необходимости выхода из главного цикла
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

// Graceful shutdown с таймаутом
// Останавливает прием новых задач, ждет завершения текущих (не более SHUTDOWN_TIMEOUT)
void graceful_shutdown_with_timeout() {
    Logger::info("Starting graceful shutdown sequence...");
    
    auto shutdown_start = std::chrono::steady_clock::now();
    
    // Шаг 0: дождаться окончания фонового начального сканирования (если ещё идёт)
    if (g_initial_scan_thread.joinable()) {
        Logger::info("Waiting for initial directory scan to finish...");
        g_initial_scan_thread.join();
    }
    
    // Шаг 1: Останавливаем монитор (если еще не остановлен)
    if (g_monitor) {
        Logger::info("Stopping monitor to prevent new task submissions...");
        g_monitor->stop();
    }
    
    // Шаг 2: Ждем завершения активных задач с таймаутом
    Logger::info(std::format("Waiting for active tasks to complete (timeout: {} seconds)...", 
                             SHUTDOWN_TIMEOUT.count()));
    
    while (g_pool && g_pool->active_count() > 0) {
        auto elapsed = std::chrono::steady_clock::now() - shutdown_start;
        if (elapsed >= SHUTDOWN_TIMEOUT) {
            Logger::warning(std::format("Graceful shutdown timeout reached ({} seconds). "
                                       "Active tasks: {}. Forcing termination.",
                                       std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
                                       g_pool->active_count()));
            break;
        }
        
        size_t active = g_pool->active_count();
        size_t queued = g_pool->queue_size();
        
        if (active > 0 || queued > 0) {
            Logger::debug(std::format("Waiting for tasks: {} active, {} queued", active, queued));
        }
        
        std::this_thread::sleep_for(SHUTDOWN_CHECK_INTERVAL);
    }
    
    // Шаг 3: Останавливаем пул потоков
    if (g_pool) {
        Logger::info("Stopping thread pool...");
        g_pool->stop();
    }
    
    auto shutdown_end = std::chrono::steady_clock::now();
    auto shutdown_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        shutdown_end - shutdown_start).count();
    
    Logger::info(std::format("Graceful shutdown completed in {} ms", shutdown_duration));
}

// Горячая перезагрузка конфигурации (SIGHUP handler)
void reload_config() {
    Logger::info("Reloading configuration...");
    try {
        auto new_cfg = std::make_unique<Config>(load_config(0, nullptr));

        Logger::info("Configuration reloaded successfully");
        Logger::info(std::format("New target paths: {}", new_cfg->target_paths.size()));

        // Атомарно заменяем конфигурацию под блокировкой
        {
            std::unique_lock<std::shared_mutex> lock(g_cfg_mutex);
            g_cfg = std::move(new_cfg);
        }

        if (g_monitor) {
            // Берём копию конфига под блокировкой для безопасной передачи в монитор
            auto cfg_copy = get_config();
            if (cfg_copy) {
                g_monitor->reload_config(*cfg_copy);
                Logger::info("Monitor configuration updated via hot reload (no restart needed)");
            }
        }

        Logger::info("Configuration reload completed successfully");
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to reload configuration: {}", e.what()));
        Logger::warning("Keeping old configuration");
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
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return ::tolower(c); });
    
    // Быстрая проверка на сжатые форматы
    if (ext == "gz" || ext == "br") return false;
    
    // Проверяем расширение напрямую из конфигурации (O(n), где n - малое число расширений)
    bool ext_match = false;
    for (const auto& e : cfg.extensions) {
        if (e.size() == ext.size()) {
            std::string lower_e = e;
            std::transform(lower_e.begin(), lower_e.end(), lower_e.begin(),
                           [](unsigned char c){ return ::tolower(c); });
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
// Возвращает приоритет и кэширует размер файла через выходной параметр
TaskPriority determine_priority(const fs::path& path, uint64_t& out_size) {
    try {
        auto size = fs::file_size(path);
        out_size = size;  // Кэшируем размер для последующего использования
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
        out_size = 0;
        return TaskPriority::NORMAL;
    } catch (const std::exception& e) {
        Logger::warning(std::format("Error getting file size: {} - {}", path.string(), e.what()));
        out_size = 0;
        return TaskPriority::NORMAL;
    }
}

// Перегруженная версия для обратной совместимости
TaskPriority determine_priority(const fs::path& path) {
    uint64_t dummy;
    return determine_priority(path, dummy);
}

// Задача сжатия (выполняется в пуле потоков)
void compress_task(const fs::path& path) {
    // Безопасно получаем копию конфигурации
    auto cfg = get_config();
    if (!cfg) return;

    // Получаем индекс переопределения для конкретного пути
    auto override_idx = get_folder_override_index(*cfg, path.string());

    // I/O троттлинг - задержка между файлами (с учетом переопределений для папки)
    int io_delay = override_idx.has_value() && cfg->folder_overrides[*override_idx].io_delay_us.has_value()
        ? *cfg->folder_overrides[*override_idx].io_delay_us
        : cfg->io_delay_us;
    if (io_delay > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(io_delay));
    }

    g_metrics.total_tasks++;
    auto start = std::chrono::steady_clock::now();

    Logger::info(std::format("Processing task for: {}", path.string()));

    if (!should_compress(path, *cfg)) {
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

    // Проверка минимального размера файла (ТЗ §4)
    // Фактический порог = max(захаркоженный минимум, настраиваемый порог)
    const size_t effective_min = std::max(Config::MIN_COMPRESS_SIZE, cfg->optimal_min_compress_size);
    if (original_size > 0 && original_size < effective_min) {
        Logger::debug(std::format("Skipping file {}: size {} bytes < minimum threshold ({} bytes)",
                                  path.string(), original_size, effective_min));
        g_metrics.files_skipped_small++;
        g_metrics.bytes_skipped_small += original_size;
        g_metrics.completed_tasks++;
        // Удаляем устаревшие сжатые копии — файл стал слишком мал для сжатия
        Compressor::safe_remove_compressed(path);
        return;
    }

    if (!cfg->dry_run) {
        Logger::info(std::format("Compressing: {}", path.string()));

        bool gzip_success = false;
        bool brotli_success = false;

        // Определяем уровни сжатия с учетом переопределений для папки
        int gzip_level = override_idx.has_value() && cfg->folder_overrides[*override_idx].compression_level_gzip.has_value()
            ? *cfg->folder_overrides[*override_idx].compression_level_gzip
            : cfg->gzip_level;
        int brotli_level = override_idx.has_value() && cfg->folder_overrides[*override_idx].compression_level_brotli.has_value()
            ? *cfg->folder_overrides[*override_idx].compression_level_brotli
            : cfg->brotli_level;

        // === ОДНОКРАТНОЕ ЧТЕНИЕ (ТЗ §3.2.4) ===
        // Открываем файл один раз, читаем в буфер, сжимаем из буфера оба формата
        int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
        if (fd < 0) {
            Logger::error(std::format("Failed to open file for reading: {} - {}", path.string(), strerror(errno)));
            g_metrics.failed_tasks++;
            return;
        }

        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            Logger::error(std::format("Not a regular file: {}", path.string()));
            g_metrics.failed_tasks++;
            return;
        }

        size_t file_size = static_cast<size_t>(st.st_size);

        // Определяем размер чанка на основе кэша CPU (ТЗ §3.2.9)
        // CacheInfo уже определён в main() — используем глобальное значение
        size_t chunk_size = g_cache.optimal_chunk_size();

        // Порог переключения: файлы <= chunk_size — one-shot, > chunk_size — streaming
        // Ограничиваем максимум размером буфера пула (256 КБ по умолчанию) для защиты от EFAULT
        constexpr size_t BUFFER_CAPACITY = 262144;  // ByteBufferPool BASE_BUFFER_SIZE
        size_t effective_chunk = std::min(chunk_size, BUFFER_CAPACITY);

        bool use_streaming = (file_size > effective_chunk);

        if (!use_streaming) {
            // === ONE-SHOT: файл помещается в память целиком ===
            // Выделяем буфер из пула (переиспользуется между файлами)
            uint8_t* buffer = buffer_pool().allocate_raw();
            if (!buffer) {
                close(fd);
                Logger::error(std::format("Failed to allocate buffer for reading: {}", path.string()));
                g_metrics.failed_tasks++;
                return;
            }

            // Чтение файла ОДИН раз
            ssize_t total_read = 0;
            size_t remaining = file_size;
            uint8_t* ptr = buffer;
            while (remaining > 0) {
                ssize_t n = read(fd, ptr, remaining);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    Logger::error(std::format("Failed to read file {}: {} - {}", path.string(), remaining, strerror(errno)));
                    break;
                }
                if (n == 0) break;  // EOF
                ptr += n;
                total_read += n;
                remaining -= n;
            }
            close(fd);

            if (static_cast<size_t>(total_read) != file_size) {
                Logger::warning(std::format("Partial read: expected {} bytes, got {} bytes for {}", file_size, total_read, path.string()));
            }

            // === RACE CONDITION CHECK (ТЗ §3.1.8) ===
            // После чтения файла проверяем, что он не был изменён/удалён/перемещён
            {
                struct stat post_stat;
                if (lstat(path.c_str(), &post_stat) != 0) {
                    // Файл удалён во время чтения — отменяем сжатие
                    Logger::warning(std::format("Race condition: file {} deleted during read, compression cancelled", path.string()));
                    buffer_pool().release_raw(buffer);
                    g_metrics.failed_tasks++;
                    return;
                }
                if (post_stat.st_mtime != st.st_mtime) {
                    // Файл изменён во время чтения — отменяем сжатие
                    Logger::debug(std::format("Race condition: file {} modified during read (mtime changed), compression cancelled", path.string()));
                    buffer_pool().release_raw(buffer);
                    g_metrics.failed_tasks++;
                    return;
                }
                if (post_stat.st_ino != st.st_ino) {
                    // Inode изменился — файл был перемещён/заменён
                    Logger::debug(std::format("Race condition: file {} inode changed during read, compression cancelled", path.string()));
                    buffer_pool().release_raw(buffer);
                    g_metrics.failed_tasks++;
                    return;
                }
                // Проверяем, что размер не упал ниже порога во время чтения
                size_t post_size = static_cast<size_t>(post_stat.st_size);
                if (post_size < effective_min) {
                    Logger::debug(std::format("Race condition: file {} size dropped below threshold ({} < {}) during read, compression cancelled",
                                               path.string(), post_size, effective_min));
                    buffer_pool().release_raw(buffer);
                    // Удаляем stale-копии, если они есть
                    Compressor::safe_remove_compressed(path);
                    g_metrics.completed_tasks++;
                    return;
                }
            }

            // Сжатие из одного буфера (данные остаются в L3 кэше CPU)
            bool prefer_brotli = (cfg->algorithms == "all" || cfg->algorithms == "brotli");

            if (prefer_brotli && (cfg->algorithms == "all" || cfg->algorithms == "brotli")) {
                brotli_success = Compressor::compress_brotli_from_memory(buffer, total_read, path.string() + ".br", brotli_level);
                if (brotli_success) {
                    struct stat br_st;
                    if (lstat((path.string() + ".br").c_str(), &br_st) == 0 && S_ISREG(br_st.st_mode)) {
                        Compressor::copy_metadata(path, path.string() + ".br");
                        try { compressed_size += fs::file_size(path.string() + ".br"); } catch (...) {}
                    }
                }
            }

            if (cfg->algorithms == "all" || cfg->algorithms == "gzip") {
                gzip_success = Compressor::compress_gzip_from_memory(buffer, total_read, path.string() + ".gz", gzip_level);
                if (gzip_success) {
                    struct stat gz_st;
                    if (lstat((path.string() + ".gz").c_str(), &gz_st) == 0 && S_ISREG(gz_st.st_mode)) {
                        Compressor::copy_metadata(path, path.string() + ".gz");
                        try { compressed_size += fs::file_size(path.string() + ".gz"); } catch (...) {}
                    }
                }
            }

            if (!prefer_brotli && (cfg->algorithms == "all" || cfg->algorithms == "brotli")) {
                brotli_success = Compressor::compress_brotli_from_memory(buffer, total_read, path.string() + ".br", brotli_level);
                if (brotli_success) {
                    struct stat br_st;
                    if (lstat((path.string() + ".br").c_str(), &br_st) == 0 && S_ISREG(br_st.st_mode)) {
                        Compressor::copy_metadata(path, path.string() + ".br");
                        try { compressed_size += fs::file_size(path.string() + ".br"); } catch (...) {}
                    }
                }
            }

            buffer_pool().release_raw(buffer);
        } else {
            // === STREAMING: чанковое сжатие для больших файлов (ТЗ §21.3-Задача 2.3) ===
            // Читаем чанками, оба алгоритма обрабатывают один чанк пока он в L3
            Logger::debug(std::format("Using streaming compression for {} ({} bytes, chunk = {} bytes)",
                                       path.string(), file_size, effective_chunk));

            Compressor::GzipStreamState gz_state;
            Compressor::BrotliStreamState br_state;

            bool gzip_started = false;
            bool brotli_started = false;

            // Запускаем streaming для включённых алгоритмов
            bool prefer_brotli = (cfg->algorithms == "all" || cfg->algorithms == "brotli");

            if (prefer_brotli && (cfg->algorithms == "all" || cfg->algorithms == "brotli")) {
                brotli_started = Compressor::brotli_stream_start(br_state, brotli_level, path.string() + ".br", st.st_mode);
            }

            if (cfg->algorithms == "all" || cfg->algorithms == "gzip") {
                gzip_started = Compressor::gzip_stream_start(gz_state, gzip_level, path.string() + ".gz", st.st_mode);
            }

            if (!brotli_started && !gzip_started) {
                close(fd);
                Logger::error(std::format("No compression algorithms enabled for {}", path.string()));
                g_metrics.failed_tasks++;
                return;
            }

            // Выделяем чанк-буфер
            uint8_t* buffer = buffer_pool().allocate_raw();
            if (!buffer) {
                close(fd);
                Logger::error(std::format("Failed to allocate streaming buffer: {}", path.string()));
                g_metrics.failed_tasks++;
                return;
            }

            // Читаем и сжимаем чанками
            size_t offset = 0;
            bool stream_error = false;

            while (offset < file_size && !stream_error) {
                size_t this_chunk = std::min(effective_chunk, file_size - offset);
                ssize_t bytes_read = 0;
                size_t remaining_read = this_chunk;
                uint8_t* ptr = buffer;

                while (remaining_read > 0) {
                    ssize_t n = read(fd, ptr, remaining_read);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        Logger::error(std::format("Streaming read error at offset {}: {}", offset, strerror(errno)));
                        stream_error = true;
                        break;
                    }
                    if (n == 0) break;
                    ptr += n;
                    bytes_read += n;
                    remaining_read -= n;
                }

                if (stream_error) break;

                // Если read вернул 0 до того как мы прочитали весь файл — ошибка
                if (bytes_read == 0) {
                    if (offset < file_size) {
                        Logger::error(std::format("Premature EOF for {}: read {} of {} bytes", path.string(), offset, file_size));
                        stream_error = true;
                    }
                    break;
                }

                offset += bytes_read;
                bool is_last = (offset >= file_size);

                // Оба алгоритма обрабатывают один чанк — данные в L3 кэше
                if (brotli_started && !br_state.has_error) {
                    if (!Compressor::brotli_stream_process(br_state, buffer, bytes_read, is_last)) {
                        Logger::error(std::format("Brotli streaming error for {}", path.string()));
                        // Не ставим stream_error — пусть gzip продолжится
                    }
                }

                if (gzip_started && !gz_state.has_error) {
                    if (!Compressor::gzip_stream_process(gz_state, buffer, bytes_read, is_last)) {
                        Logger::error(std::format("Gzip streaming error for {}", path.string()));
                        // Не ставим stream_error — алгоритмы независимы
                    }
                }
            }

            close(fd);
            buffer_pool().release_raw(buffer);

            // === RACE CONDITION CHECK для streaming (ТЗ §3.1.8) ===
            // После завершения чтения всех чанков проверяем, что файл не изменился
            {
                struct stat post_stat;
                if (lstat(path.c_str(), &post_stat) != 0) {
                    Logger::warning(std::format("Race condition: streaming file {} deleted during compression, discarding results", path.string()));
                    // Файл удалён — удаляем записанные сжатые копии если они есть
                    Compressor::safe_remove_compressed(path);
                    gzip_success = false;
                    brotli_success = false;
                    g_metrics.failed_tasks++;
                    return;
                }
                if (post_stat.st_mtime != st.st_mtime) {
                    Logger::debug(std::format("Race condition: streaming file {} modified during compression (mtime changed), discarding results", path.string()));
                    // Файл изменён — удаляем записанные сжатые копии (они содержат устаревшие данные)
                    Compressor::safe_remove_compressed(path);
                    gzip_success = false;
                    brotli_success = false;
                    g_metrics.failed_tasks++;
                    return;
                }
                if (post_stat.st_ino != st.st_ino) {
                    Logger::debug(std::format("Race condition: streaming file {} inode changed during compression, discarding results", path.string()));
                    // Inode изменился — файл перемещён/заменён, удаляем сжатые копии
                    Compressor::safe_remove_compressed(path);
                    gzip_success = false;
                    brotli_success = false;
                    g_metrics.failed_tasks++;
                    return;
                }
                size_t post_size = static_cast<size_t>(post_stat.st_size);
                if (post_size < effective_min) {
                    Logger::debug(std::format("Race condition: streaming file {} size dropped below threshold ({} < {}) during compression, discarding results",
                                               path.string(), post_size, effective_min));
                    Compressor::safe_remove_compressed(path);
                    gzip_success = false;
                    brotli_success = false;
                    g_metrics.completed_tasks++;
                    return;
                }
            }

            // Каждый алгоритм оценивается независимо
            if (gzip_started && !gz_state.has_error) gzip_success = true;
            if (brotli_started && !br_state.has_error) brotli_success = true;

            // Копируем метаданные на сжатые файлы
            if (gzip_success) {
                struct stat gz_st;
                if (lstat((path.string() + ".gz").c_str(), &gz_st) == 0 && S_ISREG(gz_st.st_mode)) {
                    Compressor::copy_metadata(path, path.string() + ".gz");
                    try { compressed_size += fs::file_size(path.string() + ".gz"); } catch (...) {}
                }
            }
            if (brotli_success) {
                struct stat br_st;
                if (lstat((path.string() + ".br").c_str(), &br_st) == 0 && S_ISREG(br_st.st_mode)) {
                    Compressor::copy_metadata(path, path.string() + ".br");
                    try { compressed_size += fs::file_size(path.string() + ".br"); } catch (...) {}
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
        // Dry-run режим: показываем детальную информацию без реального сжатия
        Logger::info(std::format("[DRY RUN] Would compress: {}", path.string()));
        Logger::info(std::format("[DRY RUN] Original size: {} bytes", original_size));

        // Оцениваем потенциальный размер после сжатия (эмпирические коэффициенты)
        // gzip обычно даёт 60-70% сжатия для текста, brotli 70-80%
        uint64_t estimated_gzip_size = original_size * 35 / 100;  // ~65% экономия
        uint64_t estimated_brotli_size = original_size * 25 / 100; // ~75% экономия

        if (cfg->algorithms == "all") {
            Logger::info(std::format("[DRY RUN] Estimated gzip size: {} bytes (~{}% savings)",
                                     estimated_gzip_size, (100 - estimated_gzip_size * 100 / (original_size > 0 ? original_size : 1))));
            Logger::info(std::format("[DRY RUN] Estimated brotli size: {} bytes (~{}% savings)",
                                     estimated_brotli_size, (100 - estimated_brotli_size * 100 / (original_size > 0 ? original_size : 1))));
            Logger::info(std::format("[DRY RUN] Total estimated savings: {} bytes (~{}%)",
                                     original_size - (estimated_gzip_size + estimated_brotli_size),
                                     (100 - (estimated_gzip_size + estimated_brotli_size) * 100 / (original_size > 0 ? original_size : 1))));
        } else if (cfg->algorithms == "gzip") {
            Logger::info(std::format("[DRY RUN] Estimated gzip size: {} bytes (~{}% savings)",
                                     estimated_gzip_size, (100 - estimated_gzip_size * 100 / (original_size > 0 ? original_size : 1))));
            Logger::info(std::format("[DRY RUN] Total estimated savings: {} bytes", original_size - estimated_gzip_size));
        } else if (cfg->algorithms == "brotli") {
            Logger::info(std::format("[DRY RUN] Estimated brotli size: {} bytes (~{}% savings)",
                                     estimated_brotli_size, (100 - estimated_brotli_size * 100 / (original_size > 0 ? original_size : 1))));
            Logger::info(std::format("[DRY RUN] Total estimated savings: {} bytes", original_size - estimated_brotli_size));
        }

        g_metrics.completed_tasks++;
        g_metrics.bytes_processed += original_size;
        g_metrics.bytes_compressed += (cfg->algorithms == "all") ?
                                       (estimated_gzip_size + estimated_brotli_size) :
                                       (cfg->algorithms == "gzip" ? estimated_gzip_size : estimated_brotli_size);
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    Logger::debug(std::format("Task completed in {} ms", duration));
}

// Задача удаления сжатых копий
void delete_task(const fs::path& path) {
    auto cfg = get_config();
    if (!cfg) return;
    if (!cfg->dry_run) {
        Logger::info(std::format("Removing compressed copies for: {}", path.string()));
        // Используем безопасное удаление с проверками
        Compressor::safe_remove_compressed(path);
    } else {
        // Dry-run режим для удаления
        Logger::info(std::format("[DRY RUN] Would remove compressed copies for: {}", path.string()));
        
        // Проверяем какие файлы были бы удалены
        fs::path gz = path.string() + ".gz";
        fs::path br = path.string() + ".br";
        
        try {
            struct stat st;
            if (lstat(gz.c_str(), &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
                Logger::info(std::format("[DRY RUN] Would remove: {} ({} bytes)", gz.string(), st.st_size));
            }
            if (lstat(br.c_str(), &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
                Logger::info(std::format("[DRY RUN] Would remove: {} ({} bytes)", br.string(), st.st_size));
            }
        } catch (const std::exception& e) {
            Logger::warning(std::format("[DRY RUN] Error checking files: {}", e.what()));
        }
    }
}

int main(int argc, char* argv[]) {
    // Загрузка конфигурации
    g_cfg = std::make_unique<Config>(load_config(argc, argv));

    // EDGE-2: Проверка существования целевых директорий (ДО инициализации ресурсов)
    for (const auto& path_str : g_cfg->target_paths) {
        struct stat st;
        if (lstat(path_str.c_str(), &st) != 0) {
            fprintf(stderr, "Error: Directory does not exist: %s\n", path_str.c_str());
            return 2;
        }
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "Error: Path is not a directory: %s\n", path_str.c_str());
            return 2;
        }
        if (S_ISLNK(st.st_mode)) {
            fprintf(stderr, "Error: Path is a symlink (potential attack): %s\n", path_str.c_str());
            return 2;
        }
    }

    // Инициализация логгера ДО сброса привилегий (чтобы логи писались от root)
    Logger::init("mh-compressor-manager", g_cfg->debug);
    Logger::info("Starting mh-compressor-manager");

    // Определение параметров кэша CPU для оптимизации буферов (ТЗ §3.2.9)
    g_cache = CacheInfo::detect();
    Logger::info(std::format("CPU cache: L3 = {} МБ, потоков = {}, буфер на поток = {} КБ",
                              g_cache.l3_total / (1024 * 1024),
                              g_cache.thread_count,
                              g_cache.optimal_buffer_size() / 1024));

    Logger::info(std::format("Target paths: {}", g_cfg->target_paths.size()));
    for (const auto& p : g_cfg->target_paths) {
        Logger::info(std::format("  - {}", p));
    }

    // Валидация конфигурации
    if (g_cfg->target_paths.empty()) {
        Logger::error("No target paths configured, exiting");
        return 1;
    }

    // Инициализация безопасной обработки сигналов через signalfd
    // ВАЖНО: Должно быть ДО seccomp, т.к. seccomp блокирует signalfd4 syscall
    if (!init_signal_fd()) {
        Logger::error("Failed to initialize signal handling, falling back to basic signals");
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGHUP, signal_handler);
    }

    // Сброс привилегий (только если запущены от root)
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
    } else {
        Logger::debug("Not running as root, skipping privilege drop and seccomp");
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

    // Сообщаем systemd о готовности СРАЗУ после запуска монитора, ДО seccomp.
    // sd_notify использует Unix-сокет (socket/connect/sendmsg), которые seccomp блокирует.
    // Монитор уже запущен и готов обрабатывать события — сервис действительно готов.
    sd_notify(0, "READY=1");
    Logger::info("Service ready (sd_notify sent); initial filesystem scan runs in background");

    g_initial_scan_thread = std::thread([]() {
        if (g_monitor) {
            g_monitor->scan_existing_files();
        }
    });

    // Инициализация seccomp ПОСЛЕ sd_notify (seccomp блокирует socket/connect/sendmsg)
    if (security::is_running_as_root() && g_cfg->enable_seccomp) {
        Logger::info("Initializing seccomp sandbox...");
        if (!security::init_seccomp()) {
            Logger::warning("Failed to initialize seccomp, continuing without sandbox");
        } else {
            Logger::info("Seccomp sandbox active");
        }
    }

    // Главный цикл с использованием epoll для обработки сигналов через signalfd
    int epfd = -1;  // Объявляем здесь чтобы был виден в catch
    try {
        // Создаём epoll дескриптор ОДИН раз перед циклом
        if (g_signal_fd >= 0) {
            epfd = epoll_create1(EPOLL_CLOEXEC);
            if (epfd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = g_signal_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, g_signal_fd, &ev);
                Logger::info("Epoll FD initialized for signal handling");
            } else {
                Logger::warning("Failed to create epoll FD, using fallback sleep");
            }
        }

        // Счётчик для периодического watchdog уведомления
        int watchdog_counter = 0;
        constexpr int WATCHDOG_INTERVAL = 30;  // Отправлять WATCHDOG каждые 30 секунд

        while (g_running) {
            // Проверяем необходимость перезагрузки конфигурации (атомарная операция)
            if (g_reload_config.load()) {
                g_reload_config.store(false);
                reload_config();
            }

            // Используем epoll для ожидания сигналов (если инициализирован)
            if (epfd >= 0) {
                struct epoll_event events[1];
                int nfds = epoll_wait(epfd, events, 1, 1000);  // Таймаут 1 секунда

                if (nfds > 0 && events[0].events & EPOLLIN) {
                    handle_signals();
                }
            } else {
                // Fallback если epoll не работает
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            // Периодическое уведомление systemd watchdog (каждые WATCHDOG_INTERVAL секунд)
            if (++watchdog_counter >= WATCHDOG_INTERVAL) {
                sd_notify(0, "WATCHDOG=1");
                watchdog_counter = 0;
            }
        }

        // Закрываем epoll дескриптор после цикла
        if (epfd >= 0) {
            close(epfd);
            epfd = -1;
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Exception in main loop: {}", e.what()));
        g_running = false;
        // Закрываем epoll дескриптор при исключении
        if (epfd >= 0) {
            close(epfd);
            epfd = -1;
        }
    }

    // Graceful shutdown с таймаутом вместо немедленной остановки
    graceful_shutdown_with_timeout();

    // Вывод метрик производительности
    g_metrics.log_summary();

    Logger::info("Service stopped gracefully");
    
    // Корректное завершение работы с очисткой глобальных указателей
    // (graceful_shutdown_with_timeout уже остановил monitor и pool)
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