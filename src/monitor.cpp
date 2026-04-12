#include "monitor.h"
#include "logger.h"
#include "security.h"
#include "i18n.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif
#include <algorithm>
#include <unordered_set>
#include <cstring>
#include <cerrno>
#include <climits>      // Для PATH_MAX
#include <map>          // Для хранения cookie событий перемещения
#include <shared_mutex> // Для shared_lock

// Структура для отслеживания событий перемещения
struct MoveEvent {
    std::string path;
    std::chrono::steady_clock::time_point timestamp;
};

// Реализация методов класса InotifyFd
InotifyFd::InotifyFd() : fd_(-1) {}

InotifyFd::~InotifyFd() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

InotifyFd::InotifyFd(InotifyFd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

InotifyFd& InotifyFd::operator=(InotifyFd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void InotifyFd::reset(int new_fd) {
    if (fd_ >= 0) {
        close(fd_);
    }
    fd_ = new_fd;
}

Monitor::Monitor(const Config& cfg) : m_cfg(cfg), m_inotify_fd(), m_running(false) {
    // Заполняем кэш расширений исходных файлов
    for (const auto& ext : m_cfg.extensions) {
        std::string lower_ext = ext;
        std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);
        m_extensions_cache.insert(lower_ext);
    }

    update_compressed_extensions();
    Logger::info_fmt(_("Initialized monitor with %zu source extensions, %zu compressed extensions based on algorithms: %s"),
                             m_extensions_cache.size(), m_compressed_extensions.size(), m_cfg.algorithms.c_str());
}

void Monitor::update_compressed_extensions() {
    // ВНИМАНИЕ: Эта функция должна вызываться ТОЛЬКО когда m_config_mutex уже захвачен
    // ИЛИ из конструктора когда никаких блокировок нет

    // Очищаем текущий кэш расширений
    m_compressed_extensions.clear();

    // Динамически определяем расширения сжатых файлов на основе включенных алгоритмов
    std::string algorithms = m_cfg.algorithms;
    std::transform(algorithms.begin(), algorithms.end(), algorithms.begin(), ::tolower);

    bool use_gzip = (algorithms == "all" || algorithms.find("gzip") != std::string::npos);
    bool use_brotli = (algorithms == "all" || algorithms.find("brotli") != std::string::npos);

    if (use_gzip) {
        m_compressed_extensions.insert("gz");
        Logger::debug("Monitoring for .gz files (gzip algorithm enabled)");
    }
    if (use_brotli) {
        m_compressed_extensions.insert("br");
        Logger::debug("Monitoring for .br files (brotli algorithm enabled)");
    }
}

// Внутренняя версия для вызова когда mutex уже захвачен
void Monitor::update_compressed_extensions_unlocked() {
    // Не захватывает mutex - вызывается из reload_config
    m_compressed_extensions.clear();

    std::string algorithms = m_cfg.algorithms;
    std::transform(algorithms.begin(), algorithms.end(), algorithms.begin(), ::tolower);

    bool use_gzip = (algorithms == "all" || algorithms.find("gzip") != std::string::npos);
    bool use_brotli = (algorithms == "all" || algorithms.find("brotli") != std::string::npos);

    if (use_gzip) {
        m_compressed_extensions.insert("gz");
    }
    if (use_brotli) {
        m_compressed_extensions.insert("br");
    }
}

void Monitor::reload_config(const Config& new_cfg) {
    std::unique_lock<std::shared_mutex> lock(m_config_mutex);

    Logger::info("Reloading monitor configuration...");

    // Определяем новые и удалённые пути
    std::vector<std::string> old_paths = m_cfg.target_paths;

    // Обновляем конфигурацию
    m_cfg = new_cfg;

    // Обновляем кэш расширений исходных файлов
    m_extensions_cache.clear();
    for (const auto& ext : m_cfg.extensions) {
        std::string lower_ext = ext;
        std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);
        m_extensions_cache.insert(lower_ext);
    }

    // Обновляем кэш расширений сжатых файлов (unlocked версия чтобы избежать deadlock)
    update_compressed_extensions_unlocked();

    Logger::info_fmt(_("Monitor configuration reloaded: %zu source extensions, %zu compressed extensions"),
                             m_extensions_cache.size(), m_compressed_extensions.size());

    // Пересоздаём inotify watches для новых путей
    // Снимаем блокировку конфигурации чтобы add_watch_recursive мог захватить shared_lock
    lock.unlock();

    // Определяем новые пути
    std::vector<std::string> new_paths;
    for (const auto& p : m_cfg.target_paths) {
        bool found = false;
        for (const auto& op : old_paths) {
            if (p == op) { found = true; break; }
        }
        if (!found) new_paths.push_back(p);
    }

    // Добавляем watches для новых путей
    for (const auto& path : new_paths) {
        Logger::info_fmt(_("Adding watch for new path: %s"), path.c_str());
        add_watch_recursive(fs::path(path));
    }

    // Удаляем watches для удалённых путей
    for (const auto& op : old_paths) {
        bool found = false;
        for (const auto& np : m_cfg.target_paths) {
            if (op == np) { found = true; break; }
        }
        if (!found) {
            Logger::info_fmt(_("Removing watch for old path: %s"), op.c_str());
            remove_watch_recursive(fs::path(op));
        }
    }

    if (!new_paths.empty() || old_paths.size() != m_cfg.target_paths.size()) {
        Logger::info_fmt(_("Watches updated: %zu added, %zu removed"),
                                 new_paths.size(), old_paths.size() - m_cfg.target_paths.size() + new_paths.size());
    }
}

Monitor::~Monitor() {
    stop();
}

void Monitor::start() {
    int fd = inotify_init();
    if (fd < 0) {
        Logger::error("Failed to init inotify");
        return;
    }
    m_inotify_fd.reset(fd);

    // Запускаем основной поток мониторинга
    m_running = true;
    m_thread = std::thread(&Monitor::run, this);
    Logger::info("Monitor started (inotify ready, watches will be added asynchronously)");

    // Обход директорий и добавление watch — в отдельном потоке (joinable, не detached)
    // Копируем target_paths под блокировкой для защиты от data race
    std::vector<std::string> watch_paths;
    {
        std::shared_lock<std::shared_mutex> cfg_lock(m_config_mutex);
        watch_paths = m_cfg.target_paths;
    }
    m_watch_thread = std::thread([this, watch_paths = std::move(watch_paths)]() mutable {
        for (const auto& path : watch_paths) {
            add_watch_recursive(fs::path(path));
        }
        Logger::info("All directory watches added");
    });
}

void Monitor::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    // Ждём завершения обхода директорий — предотвращаем use-after-free
    if (m_watch_thread.joinable()) m_watch_thread.join();
    // Ждём завершения всех rescan-потоков — предотвращаем use-after-free
    {
        std::lock_guard<std::mutex> lock(m_rescan_threads_mutex);
        for (auto& t : m_rescan_threads) {
            if (t.joinable()) t.join();
        }
        m_rescan_threads.clear();
    }
    // m_inotify_fd закроется автоматически в деструкторе RAII
    Logger::info("Monitor stopped");
}

void Monitor::scan_existing_files() {
    Logger::info("Starting initial scan of existing files...");
    int scanned = 0;
    int to_compress = 0;
    int enqueued = 0;
    int missing_compressed = 0;  // Счётчик файлов с отсутствующими сжатыми версиями
    
    for (const auto& path_str : m_cfg.target_paths) {
        fs::path base_path(path_str);
        
        // === КРИТИЧЕСКАЯ БЕЗОПАСНОСТЬ: Защита от symlink атак на базовый путь ===
        // Используем lstat вместо fs::exists/fs::is_directory для предотвращения TOCTOU
        struct stat st;
        if (lstat(base_path.c_str(), &st) != 0) {
            Logger::warning_fmt(_("Path does not exist or inaccessible: %s"), path_str.c_str());
            continue;
        }
        
        // Проверка: базовый путь не должен быть symlink - это потенциальная атака
        if (S_ISLNK(st.st_mode)) {
            Logger::error_fmt(_("SECURITY: Base path is a symlink (potential attack): %s"), path_str.c_str());
            continue;
        }
        
        if (!S_ISDIR(st.st_mode)) {
            Logger::warning_fmt(_("Path is not a directory: %s"), path_str.c_str());
            continue;
        }
        
        try {
            // skip_symlinks предотвращает следование за symlink при обходе
            // Примечание: skip_symlinks доступен в C++20+
            for (const auto& entry : fs::recursive_directory_iterator(base_path, 
                    fs::directory_options::skip_permission_denied)) {
                
                // Дополнительная проверка через lstat для каждого файла
                if (lstat(entry.path().c_str(), &st) != 0) {
                    continue;
                }
                
                // Пропускаем symlinkи - потенциальная атака
                if (S_ISLNK(st.st_mode)) {
                    Logger::debug_fmt(_("Skipping symlink: %s (potential security risk)"), entry.path().string().c_str());
                    continue;
                }
                
                if (S_ISREG(st.st_mode)) {
                    std::string filepath = entry.path().string();
                    // Проверка имени файла на опасные символы (ТЗ §8.4)
                    if (!security::validate_filename(entry.path().filename().string())) {
                        Logger::warning_fmt(_("Skipping file with invalid name during scan: %s"), filepath.c_str());
                        continue;
                    }
                    if (is_target_extension(filepath)) {
                        scanned++;
                        if (m_on_compress) {
                            fs::path gz = entry.path().string() + ".gz";
                            fs::path br = entry.path().string() + ".br";
                            bool need_compress = false;
                            bool missing_gz = false;
                            bool missing_br = false;
                            
                            try {
                                // Безопасное получение времени модификации через lstat (не следует за symlink)
                                struct stat src_st{};
                                struct stat gz_st{};
                                struct stat br_st{};

                                if (lstat(entry.path().c_str(), &src_st) != 0) {
                                    continue;
                                }

                                // === STALE-BY-SIZE DETECTION (ТЗ §3.1.6) ===
                                // Если исходный файл ниже порога сжатия — удаляем stale-копии
                                const size_t effective_min = std::max(Config::MIN_COMPRESS_SIZE, m_cfg.optimal_min_compress_size);
                                size_t src_size = static_cast<size_t>(src_st.st_size);
                                if (src_size < effective_min) {
                                    bool removed_any = false;
                                    if (lstat(gz.c_str(), &gz_st) == 0 && S_ISREG(gz_st.st_mode) && !S_ISLNK(gz_st.st_mode)) {
                                        if (unlink(gz.c_str()) == 0) {
                                            Logger::info_fmt(_("Removed stale gzip copy: %s (original %zu bytes < threshold %zu bytes)"),
                                                entry.path().string().c_str(), src_size, effective_min);
                                            removed_any = true;
                                        }
                                    }
                                    if (lstat(br.c_str(), &br_st) == 0 && S_ISREG(br_st.st_mode) && !S_ISLNK(br_st.st_mode)) {
                                        if (unlink(br.c_str()) == 0) {
                                            Logger::info_fmt(_("Removed stale brotli copy: %s (original %zu bytes < threshold %zu bytes)"),
                                                entry.path().string().c_str(), src_size, effective_min);
                                            removed_any = true;
                                        }
                                    }
                                    if (removed_any) {
                                        // Счётчик будет учтён при анализе логов
                                    }
                                    // Не сжимаем файл — он ниже порога
                                    continue;
                                }

                                auto src_time = std::chrono::system_clock::from_time_t(src_st.st_mtime);

                                // Безопасная проверка сжатых файлов через lstat (не следует за symlink)
                                bool gz_exists = (lstat(gz.c_str(), &gz_st) == 0 && !S_ISLNK(gz_st.st_mode) && S_ISREG(gz_st.st_mode));
                                bool br_exists = (lstat(br.c_str(), &br_st) == 0 && !S_ISLNK(br_st.st_mode) && S_ISREG(br_st.st_mode));
                                
                                // Проверяем наличие сжатых версий
                                missing_gz = !gz_exists;
                                missing_br = !br_exists;
                                
                                auto gz_time = gz_exists ? std::chrono::system_clock::from_time_t(gz_st.st_mtime) : decltype(src_time)::min();
                                auto br_time = br_exists ? std::chrono::system_clock::from_time_t(br_st.st_mtime) : decltype(src_time)::min();
                                
                                bool gz_ok = gz_exists && (gz_time >= src_time);
                                bool br_ok = br_exists && (br_time >= src_time);
                                
                                if (!gz_ok || !br_ok) {
                                    need_compress = true;
                                }
                                
                                // Логирование отсутствующих сжатых файлов
                                if (missing_gz || missing_br) {
                                    missing_compressed++;
                                    std::string missing_files;
                                    if (missing_gz) missing_files += ".gz";
                                    if (missing_br && !missing_gz) missing_files += ".br";
                                    else if (missing_br) missing_files += " and .br";
                                    
                                    Logger::info_fmt(_("Missing compressed file(s) for %s: queued for re-compression (%s)"),
                                        entry.path().string().c_str(), missing_files.c_str());
                                }
                            } catch (const fs::filesystem_error& e) {
                                Logger::debug_fmt(_("Error checking times for %s: %s"),
                                    entry.path().string().c_str(), e.what());
                                need_compress = true;
                            }
                            
                            if (need_compress) {
                                // Rate limiter НЕ применяется к initial scan — это фоновая обработка, не DoS
                                to_compress++;
                                if (m_on_compress) {
                                    m_on_compress(entry.path());
                                    enqueued++;
                                }
                            }
                        }
                    }
                }
            }
        } catch (const fs::filesystem_error& e) {
            Logger::warning_fmt(_("Directory access error: %s"), e.what());
        } catch (const std::exception& e) {
            Logger::warning_fmt(_("Unexpected error during directory scan: %s"), e.what());
        }
    }
    
    Logger::info_fmt(_("Initial scan completed: %d files scanned, %d queued for compression (%d with missing compressed versions)"),
                             scanned, enqueued, missing_compressed);
}

void Monitor::set_task_handler(std::function<void(const fs::path&)> handler) {
    m_on_compress = handler;
}

void Monitor::set_delete_handler(std::function<void(const fs::path&)> handler) {
    m_on_delete = handler;
}

void Monitor::add_watch_recursive(const fs::path& base_path) {
    add_watch_recursive_impl(base_path, 0);
}

void Monitor::add_watch_recursive_impl(const fs::path& base_path, size_t depth) {
    // Ограничение глубины рекурсии для защиты от DoS-атаки через глубокие директории
    if (depth > MAX_RECURSION_DEPTH) {
        Logger::warning_fmt(_("Maximum recursion depth (%zu) exceeded at: %s - skipping subdirectories"),
                                     MAX_RECURSION_DEPTH, base_path.string().c_str());
        return;
    }
    
    // === КРИТИЧЕСКАЯ БЕЗОПАСНОСТЬ: Используем lstat вместо fs.exists для предотвращения symlink атак ===
    struct stat st;
    if (lstat(base_path.c_str(), &st) != 0) {
        Logger::warning_fmt(_("Path does not exist or inaccessible: %s"), base_path.string().c_str());
        return;
    }
    
    // Проверка: базовый путь не должен быть symlink
    if (S_ISLNK(st.st_mode)) {
        Logger::error_fmt(_("SECURITY: Base path is a symlink (potential attack): %s"), base_path.string().c_str());
        return;
    }
    
    if (!S_ISDIR(st.st_mode)) {
        Logger::warning_fmt(_("Path is not a directory: %s"), base_path.string().c_str());
        return;
    }
    
    int wd = inotify_add_watch(m_inotify_fd.get(), base_path.c_str(),
        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd >= 0) {
        std::unique_lock<std::shared_mutex> lock(m_wd_path_map_mutex);
        m_wd_path_map[wd] = base_path.string();
        Logger::info_fmt(_("Watching directory: %s (depth: %zu)"), base_path.string().c_str(), depth);
    } else {
        int err = errno;
        if (err == EACCES) {
            Logger::warning_fmt(_("Permission denied, cannot watch: %s (check file permissions, SELinux, or container mounts)"), base_path.string().c_str());
        } else if (err == EPERM) {
            Logger::warning_fmt(_("Operation not permitted for: %s (check SELinux context, seccomp, or container security)"), base_path.string().c_str());
        } else if (err == ENOENT) {
            Logger::warning_fmt(_("Directory removed before watch could be added: %s"), base_path.string().c_str());
        } else if (err == ENOSPC) {
            Logger::error_fmt(_("Inotify watch limit exceeded for: %s (increase fs.inotify.max_user_watches)"), base_path.string().c_str());
        } else {
            Logger::error_fmt(_("Failed to add watch for: %s: %s"), base_path.string().c_str(), strerror(err));
        }
    }

    try {
        // Используем directory_iterator (не recursive) — рекурсия обеспечивается
        // вызовом add_watch_recursive_impl для каждой поддиректории
        for (const auto& entry : fs::directory_iterator(base_path,
                fs::directory_options::skip_permission_denied)) {
            // Проверяем symlink_status чтобы НЕ следовать за symlink
            auto symlink_st = entry.symlink_status();
            if (symlink_st.type() == fs::file_type::directory) {
                // Дополнительная проверка через lstat для каждой директории
                if (lstat(entry.path().c_str(), &st) == 0 && !S_ISLNK(st.st_mode) && S_ISDIR(st.st_mode)) {
                    int wd_sub = inotify_add_watch(m_inotify_fd.get(), entry.path().c_str(),
                        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
                    if (wd_sub >= 0) {
                        std::unique_lock<std::shared_mutex> lock(m_wd_path_map_mutex);
                        m_wd_path_map[wd_sub] = entry.path().string();
                    } else {
                        int err = errno;
                        if (err == EACCES) {
                            Logger::warning_fmt(_("Permission denied, cannot watch: %s (owner=%u:%u, mode=%o)",
                                entry.path().string().c_str(),
                                st.st_uid, st.st_gid, st.st_mode & 07777));
                        } else if (err == EPERM) {
                            Logger::warning_fmt(_("Operation not permitted for: %s (check seccomp or container security)"), entry.path().string().c_str());
                        }
                        continue;
                    }
                    // Рекурсивный вызов с увеличением глубины
                    add_watch_recursive_impl(entry.path(), depth + 1);
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        Logger::warning_fmt(_("Directory access error: %s"), e.what());
    }
}

void Monitor::remove_watch_recursive(const fs::path& base_path) {
    std::unique_lock<std::shared_mutex> lock(m_wd_path_map_mutex);
    for (auto it = m_wd_path_map.begin(); it != m_wd_path_map.end(); ) {
        const std::string& watched_path = it->second;
        // Проверяем что путь начинается с base_path (сам base_path или поддиректория)
        if (watched_path == base_path.string() ||
            watched_path.find(base_path.string() + '/') == 0) {
            int wd = it->first;
            inotify_rm_watch(m_inotify_fd.get(), wd);
            Logger::debug_fmt(_("Removed watch: %s"), watched_path.c_str());
            it = m_wd_path_map.erase(it);
        } else {
            ++it;
        }
    }
}

void Monitor::run() {
    // ОПТИМИЗАЦИЯ: Увеличенный буфер для обработки массовых событий inotify
    // 256KB вместо 64KB для лучшей производительности при большом количестве событий
    // Также добавлена поддержка динамического увеличения при переполнении
    constexpr size_t INITIAL_BUFFER_SIZE = 262144;  // 256KB
    std::vector<char> buffer(INITIAL_BUFFER_SIZE);
    
    // ОПТИМИЗАЦИЯ: Выносим вектор событий наружу для переиспользования памяти
    struct BatchEvent {
        int wd;
        uint32_t mask;  // Добавляем маску события
        std::string name;
        uint32_t cookie;
    };
    std::vector<BatchEvent> batch_events;
    batch_events.reserve(64);  // Предварительно резервируем память
    
    auto last_debounce_check = std::chrono::steady_clock::now();
    const auto debounce_check_interval = std::chrono::milliseconds(500);
    
    // Счётчик переполнений для адаптивного увеличения буфера
    int overflow_count = 0;
    constexpr int MAX_OVERFLOW_BEFORE_RESIZE = 3;
    constexpr size_t MAX_BUFFER_SIZE = 1048576;  // 1MB максимум
    
    while (m_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int fd_val = m_inotify_fd.get();
        FD_SET(fd_val, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(fd_val + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(fd_val, &readfds)) {
            // Читаем события в цикле пока есть данные
            bool has_more_data = true;
            while (has_more_data && m_running) {
                int len = read(fd_val, buffer.data(), buffer.size());
                if (len > 0) {
                    // ОБРАБОТКА ПАКЕТАМИ: Обрабатываем все события из одного read()
                    // Это улучшает производительность при массовых событиях
                    int i = 0;
                    
                    // Очищаем вектор вместо создания нового - переиспользование памяти
                    batch_events.clear();
                    
                    while (i < len) {
                        // Проверка: достаточно ли места в буфере для заголовка события
                        if (i + sizeof(struct inotify_event) > static_cast<size_t>(len)) {
                            Logger::warning_fmt(_("inotify buffer truncated: %d bytes remaining, need at least %zu for event header"),
                                                        len - i, sizeof(struct inotify_event));
                            break;
                        }

                        struct inotify_event* event = (struct inotify_event*)&buffer[i];

                        // Проверка: достаточно ли места для имени файла
                        if (i + sizeof(struct inotify_event) + event->len > static_cast<size_t>(len)) {
                            Logger::warning(_("inotify buffer truncated: event name extends beyond buffer boundary"));
                            break;
                        }

                        if (event->len > 0) {
                            batch_events.emplace_back(event->wd, event->mask, std::string(event->name), event->cookie);
                        }
                        i += sizeof(struct inotify_event) + event->len;
                    }

                    // Пакетная обработка событий с передачей маски
                    bool overflow_detected = false;
                    for (const auto& [wd, mask, name, cookie] : batch_events) {
                        // Проверка переполнения очереди inotify
                        if (mask & IN_Q_OVERFLOW) {
                            Logger::warning(_("inotify queue overflow detected — some events may have been lost. Initiating rescan."));
                            overflow_detected = true;
                        }
                        process_event(wd, mask, name, cookie);
                    }

                    // При переполнении inotify — запускаем повторное сканирование в отдельном потоке
                    // чтобы не блокировать основной цикл мониторинга
                    if (overflow_detected && m_on_compress) {
                        Logger::info(_("Rescanning directories after inotify overflow (background)..."));

                        // Очистка завершённых rescan-потоков
                        {
                            std::lock_guard<std::mutex> lock(m_rescan_threads_mutex);
                            m_rescan_threads.erase(
                                std::remove_if(m_rescan_threads.begin(), m_rescan_threads.end(),
                                    [](std::thread& t) {
                                        if (t.joinable()) { t.join(); return true; }
                                        return false;
                                    }),
                                m_rescan_threads.end()
                            );
                        }

                        // Лимит: не более 3 одновременных rescan-потоков
                        {
                            std::lock_guard<std::mutex> lock(m_rescan_threads_mutex);
                            if (m_rescan_threads.size() >= 3) {
                                Logger::warning(_("Too many concurrent rescan threads, skipping"));
                            } else {
                                m_rescan_threads.emplace_back([this]() {
                                    try {
                                        std::vector<std::string> target_paths;
                                        {
                                            std::shared_lock<std::shared_mutex> cfg_lock(m_config_mutex);
                                            target_paths = m_cfg.target_paths;
                                        }
                                        for (const auto& path_str : target_paths) {
                                            fs::path base_path(path_str);
                                            struct stat st;
                                            if (lstat(base_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                                                for (const auto& entry : fs::recursive_directory_iterator(base_path,
                                                        fs::directory_options::skip_permission_denied)) {
                                                    if (entry.is_regular_file() && is_target_extension(entry.path().string())) {
                                                        if (m_on_compress) {
                                                            m_on_compress(entry.path());
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        Logger::info(_("Rescan completed after inotify overflow"));
                                    } catch (const std::exception& e) {
                                        Logger::error_fmt(_("Rescan after inotify overflow failed: %s"), e.what());
                                    }
                                });
                            }
                        }
                    }
                    
                    // Очищаем старые cookie перемещения (таймаут)
                    {
                        std::unique_lock<std::shared_mutex> lock(m_move_mutex);
                        auto now = std::chrono::steady_clock::now();
                        for (auto it = m_move_cookies.begin(); it != m_move_cookies.end(); ) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - it->second.timestamp).count();
                            if (elapsed > MOVE_COOKIE_TIMEOUT_MS) {
                                // Таймаут истек - файл был перемещен за пределы monitored зоны или удален
                                Logger::info_fmt(_("Move timeout expired for compressed file: %s, treating as deletion"),
                                                         it->second.path.c_str());
                                
                                // Добавляем оригинал в очередь на сжатие
                                std::string original_path = get_original_path_from_compressed(fs::path(it->second.path));
                                if (!original_path.empty()) {
                                    // Проверка rate limiting перед запуском сжатия (DoS protection)
                                    if (security::g_compression_rate_limiter.try_acquire()) {
                                        if (m_on_compress) {
                                            m_on_compress(fs::path(original_path));
                                        }
                                    } else {
                                        Logger::warning_fmt(_("Rate limit exceeded, skipping re-compression: %s"), original_path.c_str());
                                    }
                                }
                                
                                it = m_move_cookies.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    
                    // Проверяем есть ли ещё данные (неблокирующий read)
                    fd_set check_fds;
                    FD_ZERO(&check_fds);
                    FD_SET(fd_val, &check_fds);
                    struct timeval zero_tv{0, 0};
                    if (select(fd_val + 1, &check_fds, NULL, NULL, &zero_tv) <= 0) {
                        has_more_data = false;
                    }
                } else if (len < 0 && errno == EINVAL) {
                    // Буфер переполнен - увеличиваем его размер
                    overflow_count++;
                    Logger::warning_fmt(_("inotify buffer overflow detected (count: %d), events may be lost"), overflow_count);
                    
                    if (overflow_count >= MAX_OVERFLOW_BEFORE_RESIZE && buffer.size() < MAX_BUFFER_SIZE) {
                        size_t new_size = std::min(buffer.size() * 2, MAX_BUFFER_SIZE);
                        buffer.resize(new_size);
                        Logger::info_fmt(_("Increased inotify buffer to %zu bytes"), new_size);
                        overflow_count = 0;
                    }
                    break;
                } else {
                    break;
                }
            }
        }
        
        // Проверяем debounced файлы только с определенной периодичностью
        auto now = std::chrono::steady_clock::now();
        if (now - last_debounce_check >= debounce_check_interval) {
            last_debounce_check = now;

            // Собираем список файлов для сжатия ПОД блокировкой (быстрая операция)
            std::vector<std::string> expired_files;
            {
                std::lock_guard<std::mutex> lock(m_debounce_mutex);
                for (auto it = m_debounce_map.begin(); it != m_debounce_map.end(); ) {
                    if (now >= it->second) {
                        expired_files.push_back(it->first);
                        it = m_debounce_map.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            // Вызываем обработчики ВНЕ блокировки — предотвращаем deadlock
            for (const auto& path : expired_files) {
                Logger::debug_fmt(_("Debounce expired for: %s"), path.c_str());
                if (security::g_compression_rate_limiter.try_acquire()) {
                    if (m_on_compress) {
                        m_on_compress(fs::path(path));
                    }
                } else {
                    Logger::warning_fmt(_("Rate limit exceeded, skipping compression after debounce: %s"), path.c_str());
                }
            }
        }
    }
}

bool Monitor::is_target_extension(const std::string& filepath) {
    // Извлекаем имя файла из полного пути
    fs::path file_path(filepath);
    std::string filename = file_path.filename().string();

    // Проверка имени файла на null-byte инъекции и опасные символы
    if (!security::validate_filename(filename)) {
        Logger::warning_fmt(_("Invalid filename detected (possible null-byte injection): %s"), filename.c_str());
        return false;
    }

    size_t dot = filename.find_last_of('.');

    // Блокировка для безопасного чтения m_cfg
    std::shared_lock<std::shared_mutex> cfg_lock(m_config_mutex);

    // Получаем индекс переопределения для конкретного пути
    auto override_idx = get_folder_override_index(m_cfg, filepath);
    bool process_without_ext = override_idx.has_value()
        ? m_cfg.folder_overrides[*override_idx].process_files_without_extensions
        : m_cfg.process_files_without_extensions;

    // Поддержка файлов без расширений: если точки нет и включена опция process_files_without_extensions
    if (dot == std::string::npos) {
        // Файл без расширения
        if (process_without_ext) {
            // Проверяем что это не сжатый файл (.gz, .br)
            if (filename == "gz" || filename == "br") {
                return false;
            }
            // Для файлов без расширений всегда возвращаем true (будут обработаны)
            return true;
        }
        return false;  // Файлы без расширений не обрабатываются по умолчанию
    }

    if (dot >= filename.size() - 1) { cfg_lock.unlock(); return false; }  // Точка в конце имени

    std::string ext = filename.substr(dot + 1);

    // Дополнительная проверка расширения на null-байты
    if (ext.empty() || ext.find('\0') != std::string::npos) {
        cfg_lock.unlock();
        return false;
    }

    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == "gz" || ext == "br") { cfg_lock.unlock(); return false; }
    
    // Получаем список расширений для этой папки (с учетом переопределений)
    if (override_idx.has_value() && !m_cfg.folder_overrides[*override_idx].extensions.empty()) {
        // Используем переопределение расширений для этой папки
        for (const auto& target_ext : m_cfg.folder_overrides[*override_idx].extensions) {
            if (ext == target_ext) {
                return true;
            }
        }
        return false;
    } else {
        // cfg_lock уже захвачен в начале функции — используем его для доступа к кэшу
        return m_extensions_cache.count(ext) > 0;
    }
}

bool Monitor::is_compressed_extension(const std::string& filename) {
    // Проверка имени файла на null-byte инъекции и опасные символы
    if (!security::validate_filename(filename)) {
        return false;
    }
    
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot >= filename.size() - 1) return false;
    
    std::string ext = filename.substr(dot + 1);
    
    // Дополнительная проверка расширения на null-байты
    if (ext.empty() || ext.find('\0') != std::string::npos) {
        return false;
    }
    
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    // Блокировка для потокобезопасного доступа к кэшу сжатых расширений (защита от race condition)
    std::shared_lock<std::shared_mutex> lock(m_config_mutex);
    // Используем кэш сжатых расширений для быстрого поиска O(1)
    return m_compressed_extensions.count(ext) > 0;
}

std::string Monitor::get_original_path_from_compressed(const fs::path& compressed_path) {
    // Удаляем расширение .gz или .br чтобы получить путь к оригиналу
    std::string path_str = compressed_path.string();
    
    if (path_str.size() > 3 && path_str.substr(path_str.size() - 3) == ".gz") {
        return path_str.substr(0, path_str.size() - 3);
    }
    if (path_str.size() > 3 && path_str.substr(path_str.size() - 3) == ".br") {
        return path_str.substr(0, path_str.size() - 3);
    }
    
    return "";  // Не распознано как сжатый файл
}

void Monitor::process_event(int wd, uint32_t mask, const std::string& name, uint32_t cookie) {
    if (name.empty()) return;

    std::string base_path = "";
    {
        std::shared_lock<std::shared_mutex> lock(m_wd_path_map_mutex);
        auto it = m_wd_path_map.find(wd);
        if (it != m_wd_path_map.end()) {
            base_path = it->second;
        }
    }
    if (base_path.empty()) {
        Logger::debug_fmt(_("Unknown wd: %d, name: %s"), wd, name.c_str());
        return;
    }

    fs::path full_path = fs::path(base_path) / name;
    
    // === КРИТИЧЕСКАЯ БЕЗОПАСНОСТЬ: Проверка и очистка пути для защиты от Path Traversal ===
    // Нормализуем путь и проверяем что он находится внутри базовой директории
    std::string normalized_path, normalized_base;
    try {
        normalized_path = fs::weakly_canonical(full_path).string();
        normalized_base = fs::weakly_canonical(fs::path(base_path)).string();
    } catch (const fs::filesystem_error& e) {
        Logger::warning_fmt(_("Path normalization failed for %s: %s"), full_path.string().c_str(), e.what());
        return;
    }

    // Проверяем что нормализованный путь находится внутри базовой директории.
    // Важно: normalized_path == normalized_base || starts_with(normalized_base + "/")
    // Простая проверка find(normalized_base) != 0 уязвима:
    //   base="/data/logs", path="/data/logs-evil" — пройдёт проверку.
    if (normalized_path != normalized_base &&
        normalized_path.rfind(normalized_base + "/", 0) != 0) {
        Logger::warning_fmt(_("SECURITY: Path traversal attempt detected: %s (base: %s)"),
                                     normalized_path.c_str(), normalized_base.c_str());
        return;
    }
    
    // Проверяем является ли файл целевым (исходным) или сжатым
    bool is_target = is_target_extension(full_path.string());
    bool is_compressed = is_compressed_extension(name);

    // Игнорируем временные файлы сжатия (.gz.tmp, .br.tmp)
    if (name.size() > 4 && (name.substr(name.size() - 4) == ".tmp")) {
        return;
    }

    if (!is_target && !is_compressed) {
        return;
    }

    // Проверка на symlink — symlink-файлы не обрабатываем (potentially dangerous)
    struct stat path_st;
    if (lstat(full_path.c_str(), &path_st) == 0 && S_ISLNK(path_st.st_mode)) {
        Logger::debug_fmt(_("Skipping symlink event: %s"), full_path.string().c_str());
        return;
    }

    Logger::debug_fmt(_("Event detected: mask=%u, path=%s, type=%s, cookie=%u"),
                              mask, full_path.string().c_str(),
                              is_compressed ? "compressed" : "target",
                              cookie);

    // Обработка событий для сжатых файлов (.gz, .br)
    if (is_compressed) {
        if (mask & IN_DELETE) {
            // Сжатый файл удален - добавляем оригинал в очередь на сжатие
            std::string original_path = get_original_path_from_compressed(full_path);
            if (!original_path.empty()) {
                Logger::info_fmt(_("Compressed file deleted: %s, queuing original for re-compression: %s"),
                                         full_path.string().c_str(), original_path.c_str());
                // Проверка rate limiting перед запуском сжатия (DoS protection)
                if (security::g_compression_rate_limiter.try_acquire()) {
                    if (m_on_compress) {
                        m_on_compress(fs::path(original_path));
                    }
                } else {
                    Logger::warning_fmt(_("Rate limit exceeded, skipping re-compression: %s"), original_path.c_str());
                }
            }
        } else if (mask & IN_MOVED_FROM) {
            // Сохраняем cookie и путь для последующей связки с IN_MOVED_TO
            std::unique_lock<std::shared_mutex> lock(m_move_mutex);
            m_move_cookies[cookie] = {
                full_path.string(),
                std::chrono::steady_clock::now()
            };
            
            Logger::debug_fmt(_("IN_MOVED_FROM for compressed file: %s (cookie: %u)"),
                                      full_path.string().c_str(), cookie);
        } else if (mask & IN_MOVED_TO) {
            // Проверяем есть ли соответствующее событие IN_MOVED_FROM с тем же cookie
            std::unique_lock<std::shared_mutex> lock(m_move_mutex);
            auto it_cookie = m_move_cookies.find(cookie);
            if (it_cookie != m_move_cookies.end()) {
                // Это перемещение внутри monitored зоны - удаляем из списка ожидающих
                Logger::info_fmt(_("Compressed file moved within monitored directory: %s -> %s"),
                                         it_cookie->second.path.c_str(), full_path.string().c_str());
                m_move_cookies.erase(it_cookie);
            } else {
                // Файл перемещен в monitored директорию извне - можно добавить в очередь проверки
                Logger::info_fmt(_("Compressed file moved into directory from outside: %s"), full_path.string().c_str());
            }
        }
        
        return;  // Для сжатых файлов дальше не обрабатываем
    }

    // Обработка событий для целевых (исходных) файлов
    if (mask & IN_DELETE) {
        // Удаляем из debounce map чтобы избежать phantom compression
        {
            std::lock_guard<std::mutex> lock(m_debounce_mutex);
            m_debounce_map.erase(full_path.string());
        }
        if (m_on_delete) m_on_delete(full_path);
    } else if (mask & IN_MOVED_FROM) {
        // Файл был перемещён из monitored директории - удаляем сжатые копии
        Logger::info_fmt(_("File moved out of monitored directory: %s"), full_path.string().c_str());
        if (m_on_delete) m_on_delete(full_path);
    } else if (!is_compressed && (mask & (IN_MODIFY | IN_CREATE | IN_MOVED_TO))) {
        // Безопасное чтение m_cfg.debounce_delay под блокировкой
        int delay;
        {
            std::shared_lock<std::shared_mutex> cfg_lock(m_config_mutex);
            delay = m_cfg.debounce_delay;
        }
        std::lock_guard<std::mutex> lock(m_debounce_mutex);
        auto now = std::chrono::steady_clock::now();
        auto deadline = now + std::chrono::seconds(delay);
        // Ограничиваем максимальное откладывание — не более 3x debounce_delay
        // Это предотвращает starvation при непрерывной записи в файл
        auto max_delay = std::chrono::seconds(delay * 3);
        auto it = m_debounce_map.find(full_path.string());
        if (it != m_debounce_map.end()) {
            // Если файл уже в map — проверяем не достигли ли max deadline от ПЕРВОГО события
            // Сравниваем текущий deadline с now + max_delay — если уже превышен, не откладываем дальше
            if (it->second > now + max_delay) {
                // Уже отложено максимально — не откладываем дальше, deadline остаётся прежним
                return;
            }
            // Обновляем deadline, но не дальше чем now + max_delay от текущего момента
            it->second = std::min(deadline, now + max_delay);
        } else {
            // Первое событие для этого файла
            m_debounce_map[full_path.string()] = deadline;
        }
        Logger::debug_fmt(_("Scheduled compression for: %s (delay: %ds)"),
            full_path.string().c_str(), delay);
    }
}