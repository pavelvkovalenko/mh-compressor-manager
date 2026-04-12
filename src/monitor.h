#pragma once
#include "config.h"
#include <filesystem>
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <string>
#include <unordered_set>
#include <unordered_map>

namespace fs = std::filesystem;

/**
 * RAII-обертка для дескриптора inotify.
 * Гарантирует закрытие дескриптора при выходе из области видимости или исключении.
 */
class InotifyFd {
public:
    InotifyFd();
    ~InotifyFd();
    
    // Запрет копирования
    InotifyFd(const InotifyFd&) = delete;
    InotifyFd& operator=(const InotifyFd&) = delete;
    
    // Разрешение перемещения
    InotifyFd(InotifyFd&& other) noexcept;
    InotifyFd& operator=(InotifyFd&& other) noexcept;
    
    int get() const { return fd_; }
    bool is_valid() const { return fd_ >= 0; }
    void reset(int new_fd = -1);
    
private:
    int fd_;
};

class Monitor {
public:
    Monitor(const Config& cfg);
    ~Monitor();
    void start();
    void stop();
    void scan_existing_files();  // Начальное сканирование существующих файлов
    void set_task_handler(std::function<void(const fs::path&)> handler);
    void set_delete_handler(std::function<void(const fs::path&)> handler);
    
    // Динамическое обновление конфигурации (hot reload)
    void reload_config(const Config& new_cfg);

private:
    void run();
    void add_watch_recursive(const fs::path& path);
    void add_watch_recursive_impl(const fs::path& path, size_t depth);  // Внутренняя реализация с ограничением глубины
    void remove_watch_recursive(const fs::path& path);  // Удаление watches для пути
    void process_event(int wd, uint32_t mask, const std::string& name, uint32_t cookie = 0);
    bool is_target_extension(const std::string& filename);
    bool is_compressed_extension(const std::string& filename);
    std::string get_original_path_from_compressed(const fs::path& compressed_path);
    void update_compressed_extensions();  // Обновление кэша расширений сжатых файлов
    void update_compressed_extensions_unlocked();  // Внутренняя версия без блокировки (для reload_config)
    
    Config m_cfg;
    InotifyFd m_inotify_fd;  // RAII-обертка вместо сырого int
    std::atomic<bool> m_running;
    std::thread m_thread;
    std::function<void(const fs::path&)> m_on_compress;
    std::function<void(const fs::path&)> m_on_delete;
    std::map<std::string, std::chrono::steady_clock::time_point> m_debounce_map;
    std::mutex m_debounce_mutex;
    mutable std::shared_mutex m_config_mutex;  // Защита от гонок при обновлении конфигурации
    std::map<int, std::string> m_wd_path_map;
    mutable std::shared_mutex m_wd_path_map_mutex;  // Защита от гонок при добавлении/чтении watch
    std::thread m_watch_thread;  // Поток обхода директорий (ранее detached)
    std::vector<std::thread> m_rescan_threads;  // Фоновые потоки rescanning после overflow
    std::mutex m_rescan_threads_mutex;  // Защита m_rescan_threads
    std::unordered_set<std::string> m_extensions_cache;  // Кэш расширений для быстрого поиска
    std::unordered_set<std::string> m_compressed_extensions;  // Расширения сжатых файлов (.gz, .br)
    
    // Отслеживание событий перемещения по cookie
    struct MoveCookieData {
        std::string path;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::unordered_map<uint32_t, MoveCookieData> m_move_cookies;  // cookie -> данные о перемещении
    mutable std::shared_mutex m_move_mutex;  // Потокобезопасный доступ к cookie
    static constexpr uint32_t MOVE_COOKIE_TIMEOUT_MS = 500;  // Таймаут для связки событий перемещения (увеличен с 100ms для надёжности)
    static constexpr size_t MAX_RECURSION_DEPTH = 20;  // Максимальная глубина рекурсии для защиты от DoS
};