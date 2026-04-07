#include "monitor.h"
#include "logger.h"
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

Monitor::Monitor(const Config& cfg) : m_cfg(cfg), m_fd(-1), m_running(false) {
    // Кэшируем расширения в unordered_set для быстрого поиска O(1)
    for (const auto& ext : m_cfg.extensions) {
        std::string lower_ext = ext;
        std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);
        m_extensions_cache.insert(lower_ext);
    }
}

Monitor::~Monitor() {
    stop();
}

void Monitor::start() {
    m_fd = inotify_init();
    if (m_fd < 0) {
        Logger::error("Failed to init inotify");
        return;
    }

    for (const auto& path_str : m_cfg.target_paths) {
        add_watch_recursive(fs::path(path_str));
    }

    m_running = true;
    m_thread = std::thread(&Monitor::run, this);
    Logger::info("Monitor started");
}

void Monitor::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_fd >= 0) close(m_fd);
    Logger::info("Monitor stopped");
}

void Monitor::scan_existing_files() {
    Logger::info("Starting initial scan of existing files...");
    int scanned = 0;
    int to_compress = 0;
    
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
                    std::string filename = entry.path().filename().string();
                    if (is_target_extension(filename)) {
                        scanned++;
                        if (m_on_compress) {
                            fs::path gz = entry.path().string() + ".gz";
                            fs::path br = entry.path().string() + ".br";
                            bool need_compress = false;
                            
                            try {
                                auto src_time = fs::last_write_time(entry.path());
                                
                                // Безопасная проверка сжатых файлов через lstat (не следует за symlink)
                                struct stat gz_st, br_st;
                                bool gz_exists = (lstat(gz.c_str(), &gz_st) == 0 && !S_ISLNK(gz_st.st_mode));
                                bool br_exists = (lstat(br.c_str(), &br_st) == 0 && !S_ISLNK(br_st.st_mode));
                                
                                bool gz_ok = gz_exists && (fs::last_write_time(gz) >= src_time);
                                bool br_ok = br_exists && (fs::last_write_time(br) >= src_time);
                                
                                if (!gz_ok || !br_ok) {
                                    need_compress = true;
                                }
                            } catch (const fs::filesystem_error& e) {
                                Logger::debug(std::format("Error checking times for {}: {}", 
                                    entry.path().string(), e.what()));
                                need_compress = true;
                            }
                            
                            if (need_compress) {
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
    
    Logger::info(std::format("Initial scan completed: {} files scanned, {} queued for compression", 
                             scanned, to_compress));
}

void Monitor::set_task_handler(std::function<void(const fs::path&)> handler) {
    m_on_compress = handler;
}

void Monitor::set_delete_handler(std::function<void(const fs::path&)> handler) {
    m_on_delete = handler;
}

void Monitor::add_watch_recursive(const fs::path& base_path) {
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
    
    int wd = inotify_add_watch(m_fd, base_path.c_str(), 
        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ISDIR);
    if (wd >= 0) {
        m_wd_path_map[wd] = base_path.string();
        Logger::info(std::format("Watching directory: {}", base_path.string()));
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
                    int wd_sub = inotify_add_watch(m_fd, entry.path().c_str(), 
                        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ISDIR);
                    if (wd_sub >= 0) {
                        m_wd_path_map[wd_sub] = entry.path().string();
                    }
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
    
    auto last_debounce_check = std::chrono::steady_clock::now();
    const auto debounce_check_interval = std::chrono::milliseconds(500);
    
    // Счётчик переполнений для адаптивного увеличения буфера
    int overflow_count = 0;
    constexpr int MAX_OVERFLOW_BEFORE_RESIZE = 3;
    constexpr size_t MAX_BUFFER_SIZE = 1048576;  // 1MB максимум
    
    while (m_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(m_fd, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(m_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(m_fd, &readfds)) {
            // Читаем события в цикле пока есть данные
            bool has_more_data = true;
            while (has_more_data && m_running) {
                int len = read(m_fd, buffer.data(), buffer.size());
                if (len > 0) {
                    // ОБРАБОТКА ПАКЕТАМИ: Обрабатываем все события из одного read()
                    // Это улучшает производительность при массовых событиях
                    int i = 0;
                    std::vector<std::pair<int, std::string>> batch_events;
                    
                    while (i < len) {
                        struct inotify_event* event = (struct inotify_event*)&buffer[i];
                        if (event->len > 0) {
                            batch_events.emplace_back(event->wd, std::string(event->name));
                        }
                        i += sizeof(struct inotify_event) + event->len;
                    }
                    
                    // Пакетная обработка событий
                    for (const auto& [wd, name] : batch_events) {
                        process_event(wd, 0, name);
                    }
                    
                    // Проверяем есть ли ещё данные (неблокирующий read)
                    fd_set check_fds;
                    FD_ZERO(&check_fds);
                    FD_SET(m_fd, &check_fds);
                    struct timeval zero_tv{0, 0};
                    if (select(m_fd + 1, &check_fds, NULL, NULL, &zero_tv) <= 0) {
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
                    if (m_on_compress) m_on_compress(fs::path(it->first));
                    it = m_debounce_map.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

bool Monitor::is_target_extension(const std::string& filename) {
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) return false;
    
    std::string ext = filename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == "gz" || ext == "br") return false;
    
    // Используем кэш расширений для быстрого поиска O(1)
    return m_extensions_cache.count(ext) > 0;
}

void Monitor::process_event(int wd, uint32_t mask, const std::string& name) {
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
    
    if (!is_target_extension(name)) {
        return;
    }

    Logger::debug(std::format("Event detected: mask={}, path={}", mask, full_path.string()));

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