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
    int gzip_level = 9;  // Максимальное сжатие по умолчанию (статика, фоновое сжатие)
    int brotli_level = 11;  // Максимальное сжатие по умолчанию (статика, фоновое сжатие)
    int debounce_delay = 2; // seconds
    std::string config_path = "/etc/mediahive/compressor-manager.conf";

    // Минимальный размер файла для сжатия (ТЗ §4)
    // MIN_COMPRESS_SIZE = 256 байт — захардкоженный абсолютный минимум
    static constexpr size_t MIN_COMPRESS_SIZE = 256;

    // OPTIMAL_MIN_COMPRESS_SIZE — настраиваемый порог (по умолчанию 256)
    // Фактический минимум = max(MIN_COMPRESS_SIZE, optimal_min_compress_size)
    // ТЗ §4.4: "Значение по умолчанию: 256 байт"
    size_t optimal_min_compress_size = 256;

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
    int cli_min_size = -1;  // CLI override для optimal_min_compress_size (--min-size)
    bool cli_debug = false;  // CLI override для debug
    bool dry_run = false;
};

Config load_config(int argc, char* argv[]);

// Получить индекс настройки для конкретного пути (с учетом переопределений)
// Возвращает std::nullopt если нет переопределений
std::optional<size_t> get_folder_override_index(const Config& cfg, const std::string& path);
