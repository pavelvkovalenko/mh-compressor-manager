#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>

// Структура настроек для конкретной папки
struct FolderOverride {
    std::string path;
    std::optional<int> compression_level_gzip;      // Уровень сжатия gzip (переопределяет глобальный)
    std::optional<int> compression_level_brotli;    // Уровень сжатия brotli (переопределяет глобальный)
    std::vector<std::string> extensions;            // Список расширений для этой папки
    bool process_files_without_extensions = false;  // Обрабатывать файлы без расширений
    bool recursive = true;                          // Рекурсивная обработка подпапок
    std::optional<int> rate_limit;                  // Лимит операций в минуту (0 = без лимита)
    std::optional<int> io_delay_us;                 // Задержка I/O для этой папки
    std::optional<size_t> max_active_ios;           // Лимит параллельных I/O для этой папки
};

struct Config {
    std::vector<std::string> target_paths;
    bool debug = false;
    int threads = 0; // 0 = auto
    std::vector<std::string> extensions;
    std::string algorithms = "all"; // gzip, brotli, all
    int gzip_level = 9;  // Максимальное сжатие по умолчанию
    int brotli_level = 11;  // Максимальное сжатие по умолчанию
    int debounce_delay = 2; // seconds
    std::string config_path = "/etc/mediahive/compressor-manager.conf";
    
    // Ограничение I/O нагрузки (глобальные настройки)
    int io_delay_us = 0;         // Задержка между файлами (микросекунды)
    size_t max_active_ios = 0;   // Лимит параллельных I/O операций (0 = без лимита)
    
    // Безопасность
    bool drop_privileges = true;      // Сбрасывать привилегии (по умолчанию true для root)
    bool enable_seccomp = true;       // Включить песочницу seccomp (по умолчанию true)
    std::string run_as_user = "";     // Явный пользователь для сброса прав (пусто = авто)
    
    // Поддержка файлов без расширений (глобальная настройка)
    bool process_files_without_extensions = false;  // Обрабатывать файлы без расширений
    
    // Индивидуальные настройки для папок (переопределяют глобальные)
    std::vector<FolderOverride> folder_overrides;
    
    // CLI overrides
    std::vector<std::string> cli_dirs;
    std::vector<std::string> cli_exts;
    int cli_gzip_level = -1;
    int cli_brotli_level = -1;
    bool dry_run = false;
};

Config load_config(int argc, char* argv[]);

// Получить настройки для конкретного пути (с учетом переопределений)
const FolderOverride* get_folder_override(const Config& cfg, const std::string& path);
