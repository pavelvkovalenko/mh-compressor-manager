#pragma once
#include "config.h"
#include <filesystem>
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <chrono>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

class Monitor {
public:
    Monitor(const Config& cfg);
    ~Monitor();
    void start();
    void stop();
    void scan_existing_files();  // <--- ДОБАВЛЕНО: начальное сканирование
    void set_task_handler(std::function<void(const fs::path&)> handler);
    void set_delete_handler(std::function<void(const fs::path&)> handler);

private:
    void run();
    void add_watch_recursive(const fs::path& path);
    void process_event(int wd, uint32_t mask, const std::string& name);
    bool is_target_extension(const std::string& filename);
    
    Config m_cfg;
    int m_fd;
    std::atomic<bool> m_running;
    std::thread m_thread;
    std::function<void(const fs::path&)> m_on_compress;
    std::function<void(const fs::path&)> m_on_delete;
    std::map<std::string, std::chrono::steady_clock::time_point> m_debounce_map;
    std::mutex m_debounce_mutex;
    std::map<int, std::string> m_wd_path_map;
    std::unordered_set<std::string> m_extensions_cache;  // Кэш расширений для быстрого поиска
};