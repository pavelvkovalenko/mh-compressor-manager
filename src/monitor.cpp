#include "monitor.h"
#include "logger.h"
#include "security.h"
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
    update_compressed_extensions();
    Logger::info(std::format("Initialized monitor with {} compressed extensions based on algorithms: {}", 
                             m_compressed_extensions.size(), m_cfg.algorithms));
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

    Logger::info(std::format("Monitor configuration reloaded: {} source extensions, {} compressed extensions",
                             m_extensions_cache.size(), m_compressed_extensions.size()));
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

    // Запускаем поток мониторинга СРАЗУ, без ожидания обхода директорий
    m_running = true;
    m_thread = std::thread(&Monitor::run, this);
    Logger::info("Monitor started (inotify ready, watches will be added asynchronously)");

    // Обход директорий и добавление watch — в фоновом потоке
    std::thread([this]() {
        for (const auto& path_str : m_cfg.target_paths) {
            add_watch_recursive(fs::path(path_str));
        }
        Logger::info("All directory watches added");
    }).detach();
}

void Monitor::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    // m_inotify_fd закроется автоматически в деструкторе RAII
    Logger::info("Monitor stopped");
}

void Monitor::scan_existing_files() {
    Logger::info("Starting initial scan of existing files...");
    int scanned = 0;
    int to_compress = 0;
    int missing_compressed = 0;  // Счётчик файлов с отсутствующими сжатыми версиями
    
    for (const auto& path_str : m_cfg.target_paths) {
        fs::path base_path(path_str);
        
        // === КРИТИЧЕСКАЯ БЕЗОПАСНОСТЬ: Защита от symlink атак на базовый путь ===
        // Используем lstat вместо fs::exists/fs::is_directory для предотвращения TOCTOU
        struct stat st;
        if (lstat(base_path.c_str(), &st) != 0) {
            Logger::warning(std::format("Path does not exist or inaccessible: {}", path_str));
            continue;
        }
        
        // Проверка: базовый путь не должен быть symlink - это потенциальная атака
        if (S_ISLNK(st.st_mode)) {
            Logger::error(std::format("SECURITY: Base path is a symlink (potential attack): {}", path_str));
            continue;
        }
        
        if (!S_ISDIR(st.st_mode)) {
            Logger::warning(std::format("Path is not a directory: {}", path_str));
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
                    Logger::debug(std::format("Skipping symlink: {} (potential security risk)", entry.path().string()));
                    continue;
                }
                
                if (S_ISREG(st.st_mode)) {
                    std::string filepath = entry.path().string();
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
                                struct stat src_st, gz_st, br_st;
                                
                                if (lstat(entry.path().c_str(), &src_st) != 0) {
                                    continue;
                                }
                                auto src_time = std::chrono::system_clock::from_time_t(src_st.st_mtime);
                                
                                // Безопасная проверка сжатых файлов через lstat (не следует за symlink)
                                bool gz_exists = (lstat(gz.c_str(), &gz_st) == 0 && !S_ISLNK(gz_st.st_mode));
                                bool br_exists = (lstat(br.c_str(), &br_st) == 0 && !S_ISLNK(br_st.st_mode));
                                
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
                                    
                                    Logger::info(std::format("Missing compressed file(s) for {}: queued for re-compression ({})",
                                        entry.path().string(), missing_files));
                                }
                            } catch (const fs::filesystem_error& e) {
                                Logger::debug(std::format("Error checking times for {}: {}", 
                                    entry.path().string(), e.what()));
                                need_compress = true;
                            }
                            
                            if (need_compress) {
                                // Проверка rate limiting перед запуском сжатия (DoS protection)
                                if (!security::g_compression_rate_limiter.try_acquire()) {
                                    Logger::warning(std::format("Rate limit exceeded, skipping compression: {}", 
                                                                entry.path().string()));
                                    continue;
                                }
                                
                                to_compress++;
                                m_on_compress(entry.path());
                            }
                        }
                    }
                }
            }
        } catch (const fs::filesystem_error& e) {
            Logger::warning(std::format("Directory access error: {}", e.what()));
        }
    }
    
    Logger::info(std::format("Initial scan completed: {} files scanned, {} queued for compression ({} with missing compressed versions)", 
                             scanned, to_compress, missing_compressed));
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
        Logger::warning(std::format("Maximum recursion depth ({}) exceeded at: {} - skipping subdirectories", 
                                     MAX_RECURSION_DEPTH, base_path.string()));
        return;
    }
    
    // === КРИТИЧЕСКАЯ БЕЗОПАСНОСТЬ: Используем lstat вместо fs.exists для предотвращения symlink атак ===
    struct stat st;
    if (lstat(base_path.c_str(), &st) != 0) {
        Logger::warning(std::format("Path does not exist or inaccessible: {}", base_path.string()));
        return;
    }
    
    // Проверка: базовый путь не должен быть symlink
    if (S_ISLNK(st.st_mode)) {
        Logger::error(std::format("SECURITY: Base path is a symlink (potential attack): {}", base_path.string()));
        return;
    }
    
    if (!S_ISDIR(st.st_mode)) {
        Logger::warning(std::format("Path is not a directory: {}", base_path.string()));
        return;
    }
    
    int wd = inotify_add_watch(m_inotify_fd.get(), base_path.c_str(), 
        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ISDIR);
    if (wd >= 0) {
        m_wd_path_map[wd] = base_path.string();
        Logger::info(std::format("Watching directory: {} (depth: {})", base_path.string(), depth));
    } else {
        Logger::error(std::format("Failed to add watch for: {}", base_path.string()));
    }

    try {
        // skip_symlinks предотвращает следование за symlink при обходе
        for (const auto& entry : fs::recursive_directory_iterator(base_path,
                fs::directory_options::skip_permission_denied)) {
            if (entry.is_directory()) {
                // Дополнительная проверка через lstat для каждой директории
                if (lstat(entry.path().c_str(), &st) == 0 && !S_ISLNK(st.st_mode) && S_ISDIR(st.st_mode)) {
                    int wd_sub = inotify_add_watch(m_inotify_fd.get(), entry.path().c_str(), 
                        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ISDIR);
                    if (wd_sub >= 0) {
                        m_wd_path_map[wd_sub] = entry.path().string();
                    }
                    // Рекурсивный вызов с увеличением глубины
                    add_watch_recursive_impl(entry.path(), depth + 1);
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        Logger::warning(std::format("Directory access error: {}", e.what()));
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
                        struct inotify_event* event = (struct inotify_event*)&buffer[i];
                        if (event->len > 0) {
                            batch_events.emplace_back(event->wd, event->mask, std::string(event->name), event->cookie);
                        }
                        i += sizeof(struct inotify_event) + event->len;
                    }

                    // Пакетная обработка событий с передачей маски
                    for (const auto& [wd, mask, name, cookie] : batch_events) {
                        process_event(wd, mask, name, cookie);
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
                                Logger::info(std::format("Move timeout expired for compressed file: {}, treating as deletion", 
                                                         it->second.path));
                                
                                // Добавляем оригинал в очередь на сжатие
                                std::string original_path = get_original_path_from_compressed(fs::path(it->second.path));
                                if (!original_path.empty()) {
                                    // Проверка rate limiting перед запуском сжатия (DoS protection)
                                    if (security::g_compression_rate_limiter.try_acquire()) {
                                        if (m_on_compress) {
                                            m_on_compress(fs::path(original_path));
                                        }
                                    } else {
                                        Logger::warning(std::format("Rate limit exceeded, skipping re-compression: {}", original_path));
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
                    Logger::warning(std::format("inotify buffer overflow detected (count: {}), events may be lost", overflow_count));
                    
                    if (overflow_count >= MAX_OVERFLOW_BEFORE_RESIZE && buffer.size() < MAX_BUFFER_SIZE) {
                        size_t new_size = std::min(buffer.size() * 2, MAX_BUFFER_SIZE);
                        buffer.resize(new_size);
                        Logger::info(std::format("Increased inotify buffer to {} bytes", new_size));
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
            std::lock_guard<std::mutex> lock(m_debounce_mutex);
            for (auto it = m_debounce_map.begin(); it != m_debounce_map.end(); ) {
                if (now >= it->second) {
                    Logger::debug(std::format("Debounce expired for: {}", it->first));
                    // Проверка rate limiting перед запуском сжатия (DoS protection)
                    if (security::g_compression_rate_limiter.try_acquire()) {
                        if (m_on_compress) {
                            m_on_compress(fs::path(it->first));
                        }
                    } else {
                        Logger::warning(std::format("Rate limit exceeded, skipping compression after debounce: {}", 
                                                    it->first));
                    }
                    it = m_debounce_map.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

bool Monitor::is_target_extension(const std::string& filepath) {
    // Проверка имени файла на null-byte инъекции и опасные символы
    if (!security::validate_filename(filepath)) {
        Logger::warning(std::format("Invalid filename detected (possible null-byte injection): {}", filepath));
        return false;
    }
    
    // Извлекаем имя файла из полного пути
    fs::path file_path(filepath);
    std::string filename = file_path.filename().string();
    
    size_t dot = filename.find_last_of('.');
    
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
            // Блокировка для потокобезопасного доступа к кэшу расширений
            std::shared_lock<std::shared_mutex> lock(m_config_mutex);
            // Для файлов без расширений всегда возвращаем true (будут обработаны)
            return true;
        }
        return false;  // Файлы без расширений не обрабатываются по умолчанию
    }
    
    if (dot >= filename.size() - 1) return false;  // Точка в конце имени
    
    std::string ext = filename.substr(dot + 1);
    
    // Дополнительная проверка расширения на null-байты
    if (ext.empty() || ext.find('\0') != std::string::npos) {
        return false;
    }
    
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == "gz" || ext == "br") return false;
    
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
        // Блокировка для потокобезопасного доступа к кэшу расширений
        std::shared_lock<std::shared_mutex> lock(m_config_mutex);
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
    auto it = m_wd_path_map.find(wd);
    if (it != m_wd_path_map.end()) {
        base_path = it->second;
    } else {
        Logger::debug(std::format("Unknown wd: {}, name: {}", wd, name));
        return;
    }

    fs::path full_path = fs::path(base_path) / name;
    
    // === КРИТИЧЕСКАЯ БЕЗОПАСНОСТЬ: Проверка и очистка пути для защиты от Path Traversal ===
    // Нормализуем путь и проверяем что он находится внутри базовой директории
    std::string normalized_path = fs::weakly_canonical(full_path).string();
    std::string normalized_base = fs::weakly_canonical(fs::path(base_path)).string();
    
    // Проверяем что нормализованный путь начинается с базовой директории
    if (normalized_path.find(normalized_base) != 0) {
        Logger::warning(std::format("SECURITY: Path traversal attempt detected: {} (base: {})", 
                                     normalized_path, normalized_base));
        return;
    }
    
    // Проверяем является ли файл целевым (исходным) или сжатым
    bool is_target = is_target_extension(full_path.string());
    bool is_compressed = is_compressed_extension(name);
    
    if (!is_target && !is_compressed) {
        return;
    }

    Logger::debug(std::format("Event detected: mask={}, path={}, type={}, cookie={}", 
                              mask, full_path.string(), 
                              is_compressed ? "compressed" : "target",
                              cookie));

    // Обработка событий для сжатых файлов (.gz, .br)
    if (is_compressed) {
        if (mask & IN_DELETE) {
            // Сжатый файл удален - добавляем оригинал в очередь на сжатие
            std::string original_path = get_original_path_from_compressed(full_path);
            if (!original_path.empty()) {
                Logger::info(std::format("Compressed file deleted: {}, queuing original for re-compression: {}", 
                                         full_path.string(), original_path));
                // Проверка rate limiting перед запуском сжатия (DoS protection)
                if (security::g_compression_rate_limiter.try_acquire()) {
                    if (m_on_compress) {
                        m_on_compress(fs::path(original_path));
                    }
                } else {
                    Logger::warning(std::format("Rate limit exceeded, skipping re-compression: {}", original_path));
                }
            }
        } else if (mask & IN_MOVED_FROM) {
            // Сохраняем cookie и путь для последующей связки с IN_MOVED_TO
            std::unique_lock<std::shared_mutex> lock(m_move_mutex);
            m_move_cookies[cookie] = {
                full_path.string(),
                std::chrono::steady_clock::now()
            };
            
            Logger::debug(std::format("IN_MOVED_FROM for compressed file: {} (cookie: {})", 
                                      full_path.string(), cookie));
        } else if (mask & IN_MOVED_TO) {
            // Проверяем есть ли соответствующее событие IN_MOVED_FROM с тем же cookie
            std::unique_lock<std::shared_mutex> lock(m_move_mutex);
            auto it_cookie = m_move_cookies.find(cookie);
            if (it_cookie != m_move_cookies.end()) {
                // Это перемещение внутри monitored зоны - удаляем из списка ожидающих
                Logger::info(std::format("Compressed file moved within monitored directory: {} -> {}", 
                                         it_cookie->second.path, full_path.string()));
                m_move_cookies.erase(it_cookie);
            } else {
                // Файл перемещен в monitored директорию извне - можно добавить в очередь проверки
                Logger::info(std::format("Compressed file moved into directory from outside: {}", full_path.string()));
            }
        }
        
        return;  // Для сжатых файлов дальше не обрабатываем
    }

    // Обработка событий для целевых (исходных) файлов
    if (mask & IN_DELETE) {
        if (m_on_delete) m_on_delete(full_path);
    } else if (mask & IN_MOVED_FROM) {
        // Файл был перемещён из monitored директории - удаляем сжатые копии
        Logger::info(std::format("File moved out of monitored directory: {}", full_path.string()));
        if (m_on_delete) m_on_delete(full_path);
    } else if (mask & (IN_MODIFY | IN_CREATE | IN_MOVED_TO)) {
        std::lock_guard<std::mutex> lock(m_debounce_mutex);
        m_debounce_map[full_path.string()] = 
            std::chrono::steady_clock::now() + std::chrono::seconds(m_cfg.debounce_delay);
        Logger::debug(std::format("Scheduled compression for: {} (delay: {}s)", 
            full_path.string(), m_cfg.debounce_delay));
    }
}