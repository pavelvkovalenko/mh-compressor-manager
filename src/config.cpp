#include "config.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif
#include <iostream>
#include <unistd.h>
#include <charconv>
#include <cstring>

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Улучшенная функция split с поддержкой пробелов в значениях
static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> res;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        std::string t = trim(item);
        if (!t.empty()) res.push_back(t);
    }
    return res;
}

// Проверка на null-byte инъекции в имени пути
static bool has_null_byte_injection(const std::string& path) {
    // Проверяем наличие встроенных null-символов (кроме конца строки)
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '\0') {
            return true;
        }
    }
    return false;
}

// Валидация пути: проверка на опасные символы и последовательности
static bool is_path_safe(const std::string& path) {
    if (path.empty()) return false;
    
    // Проверка на null-byte инъекции
    if (has_null_byte_injection(path)) {
        return false;
    }
    
    // Проверка на path traversal
    if (path.find("..") != std::string::npos) {
        return false;
    }
    
    return true;
}

// Получить настройки для конкретного пути (с учетом переопределений)
std::optional<size_t> get_folder_override_index(const Config& cfg, const std::string& path) {
    // Ищем наиболее подходящее переопределение (наиболее длинный совпадающий префикс)
    std::optional<size_t> best_index;
    size_t best_match_len = 0;

    for (size_t i = 0; i < cfg.folder_overrides.size(); ++i) {
        const auto& override = cfg.folder_overrides[i];
        // Проверяем, начинается ли путь с пути переопределения
        if (path.size() >= override.path.size()) {
            if (path.substr(0, override.path.size()) == override.path) {
                // Дополнительная проверка: либо точное совпадение, либо следующий символ - разделитель пути
                if (path.size() == override.path.size() ||
                    path[override.path.size()] == '/') {
                    if (override.path.size() > best_match_len) {
                        best_index = i;
                        best_match_len = override.path.size();
                    }
                }
            }
        }
    }

    return best_index;
}

Config load_config(int argc, char* argv[]) {
    Config cfg;
    
    // Simple CLI parsing с проверкой диапазонов числовых параметров
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: mh-compressor-manager [options]\n"
                      << "  --config <path>   Config file path\n"
                      << "  --dir <path>      Target directory (override)\n"
                      << "  --ext <list>      Extensions (override)\n"
                      << "  --gzip-level <N>  Gzip level (1-9)\n"
                      << "  --brotli-level <N> Brotli level (1-11)\n"
                      << "  --dry-run         Dry run mode\n"
                      << "  --version         Show version\n"
                      << "  --process-without-ext  Process files without extensions\n";
            exit(0);
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "mh-compressor-manager v1.0.1\n";
            exit(0);
        }
        if (arg == "--config" && i + 1 < argc) cfg.config_path = argv[++i];
        else if (arg == "--dir" && i + 1 < argc) cfg.cli_dirs.push_back(argv[++i]);
        else if (arg == "--ext" && i + 1 < argc) {
            auto exts = split(argv[++i], ' ');
            cfg.cli_exts.insert(cfg.cli_exts.end(), exts.begin(), exts.end());
        }
        else if (arg == "--gzip-level" && i + 1 < argc) {
            const char* val = argv[++i];
            int level = 0;
            const std::size_t len = std::strlen(val);
            auto result = std::from_chars(val, val + len, level);
            if (result.ec != std::errc() || result.ptr != val + len) {
                std::cerr << "Warning: invalid gzip level format, using default 6\n";
                level = 6;
            }
            if (level < 1 || level > 9) {
                std::cerr << "Warning: gzip level must be 1-9, using default 6\n";
                level = 6;
            }
            cfg.cli_gzip_level = level;
        }
        else if (arg == "--brotli-level" && i + 1 < argc) {
            const char* val = argv[++i];
            int level = 0;
            const std::size_t len = std::strlen(val);
            auto result = std::from_chars(val, val + len, level);
            if (result.ec != std::errc() || result.ptr != val + len) {
                std::cerr << "Warning: invalid brotli level format, using default 4\n";
                level = 4;
            }
            if (level < 1 || level > 11) {
                std::cerr << "Warning: brotli level must be 1-11, using default 4\n";
                level = 4;
            }
            cfg.cli_brotli_level = level;
        }
        else if (arg == "--dry-run") cfg.dry_run = true;
        else if (arg == "--process-without-ext") cfg.process_files_without_extensions = true;
    }

    // Load INI
    std::ifstream file(cfg.config_path);
    if (!file.is_open()) {
        Logger::warning(std::format("Config file {} not found, using defaults", cfg.config_path));
    } else {
        std::string line;
        std::string current_section = "general";
        std::string current_folder_path;  // Текущий путь для секции folder_override
        
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            
            // Обработка заголовков секций
            if (line.front() == '[' && line.back() == ']') {
                std::string section_content = line.substr(1, line.size() - 2);
                
                // Проверка на секцию folder_override с путем
                if (section_content.rfind("folder_override:", 0) == 0) {
                    // Это секция переопределения для конкретной папки
                    current_section = "folder_override";
                    current_folder_path = trim(section_content.substr(16));  // Извлекаем путь после "folder_override:"
                    
                    // Валидация пути
                    if (!is_path_safe(current_folder_path)) {
                        Logger::warning(std::format("Unsafe path in folder_override section: {}, skipping", current_folder_path));
                        current_folder_path.clear();
                        continue;
                    }
                    
                    // Создаем новую запись переопределения
                    FolderOverride override;
                    override.path = current_folder_path;
                    cfg.folder_overrides.push_back(override);
                } else {
                    // Обычная секция
                    current_section = section_content;
                    current_folder_path.clear();
                }
                continue;
            }
            
            // Парсинг ключ=значение
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            
            std::string key = trim(line.substr(0, eq));
            // Значение берем как есть после '=', но trim только начальные пробелы
            // Это позволяет сохранять пробелы в конце имен путей
            std::string val = line.substr(eq + 1);
            // Удаляем только начальные пробелы и табуляции, но сохраняем конечные (для путей)
            size_t val_start = val.find_first_not_of(" \t");
            if (val_start != std::string::npos) {
                val = val.substr(val_start);
            } else {
                val.clear();
            }
            // Удаляем только завершающие CR/LF, но не обычные пробелы (они могут быть частью пути)
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) {
                val.pop_back();
            }
            
            // Обработка в зависимости от секции
            if (current_section == "general" || current_section.empty()) {
                if (key == "target_path") cfg.target_paths = split(val, ';');
                else if (key == "debug") cfg.debug = (val == "true");
                else if (key == "threads") {
                    int threads = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), threads);
                    if (result.ec != std::errc() || result.ptr != val.data() + val.size()) {
                        Logger::warning("Invalid threads value format, using auto-detect");
                        cfg.threads = 0;
                    } else {
                        // Валидация диапазона количества потоков
                        if (threads < 0 || threads > 256) {
                            Logger::warning(std::format("Invalid threads value {}, using auto-detect", threads));
                            cfg.threads = 0;
                        } else {
                            cfg.threads = threads;
                        }
                    }
                }
                else if (key == "list") {
                    auto exts = split(val, ' ');
                    cfg.extensions.insert(cfg.extensions.end(), exts.begin(), exts.end());
                }
                else if (key == "algorithms") cfg.algorithms = val;
                else if (key == "gzip_level") {
                    int level = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), level);
                    if (result.ec != std::errc() || result.ptr != val.data() + val.size()) {
                        Logger::warning("Invalid gzip_level format, using default 6");
                        level = 6;
                    } else if (level < 1 || level > 9) {
                        Logger::warning(std::format("Invalid gzip_level {}, using default 6", level));
                        level = 6;
                    }
                    cfg.gzip_level = level;
                }
                else if (key == "brotli_level") {
                    int level = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), level);
                    if (result.ec != std::errc() || result.ptr != val.data() + val.size()) {
                        Logger::warning("Invalid brotli_level format, using default 4");
                        level = 4;
                    } else if (level < 1 || level > 11) {
                        Logger::warning(std::format("Invalid brotli_level {}, using default 4", level));
                        level = 4;
                    }
                    cfg.brotli_level = level;
                }
                else if (key == "debounce_delay") {
                    int delay = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), delay);
                    if (result.ec != std::errc() || result.ptr != val.data() + val.size()) {
                        Logger::warning("Invalid debounce_delay format, using default 2");
                        delay = 2;
                    } else if (delay < 0 || delay > 60) {
                        Logger::warning(std::format("Invalid debounce_delay {}, using default 2", delay));
                        delay = 2;
                    }
                    cfg.debounce_delay = delay;
                }
                else if (key == "io_delay_us") {
                    int delay = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), delay);
                    if (result.ec != std::errc() || result.ptr != val.data() + val.size()) {
                        Logger::warning("Invalid io_delay_us format, using default 0");
                        delay = 0;
                    } else if (delay < 0 || delay > 1000000) {
                        Logger::warning(std::format("Invalid io_delay_us {}, using default 0", delay));
                        delay = 0;
                    }
                    cfg.io_delay_us = delay;
                }
                else if (key == "max_active_ios") {
                    unsigned long long value = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), value);
                    if (result.ec != std::errc() || result.ptr != val.data() + val.size()) {
                        Logger::warning("Invalid max_active_ios format, using unlimited");
                        cfg.max_active_ios = 0;
                    } else {
                        cfg.max_active_ios = value;
                        if (cfg.max_active_ios > 10000) {
                            Logger::warning(std::format("max_active_ios {} too high, limiting to 10000", cfg.max_active_ios));
                            cfg.max_active_ios = 10000;
                        }
                    }
                }
                else if (key == "drop_privileges") cfg.drop_privileges = (val == "true");
                else if (key == "enable_seccomp") cfg.enable_seccomp = (val == "true");
                else if (key == "run_as_user") cfg.run_as_user = val;
                else if (key == "process_files_without_extensions") cfg.process_files_without_extensions = (val == "true");
            }
            else if (current_section == "folder_override" && !cfg.folder_overrides.empty()) {
                // Обработка настроек для переопределения папки
                FolderOverride& override = cfg.folder_overrides.back();
                
                if (key == "compression_level_gzip") {
                    int level = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), level);
                    if (result.ec == std::errc() && result.ptr == val.data() + val.size()) {
                        if (level >= 1 && level <= 9) {
                            override.compression_level_gzip = level;
                        } else {
                            Logger::warning(std::format("Invalid gzip_level {} in folder_override, must be 1-9", level));
                        }
                    } else {
                        Logger::warning("Invalid compression_level_gzip format in folder_override");
                    }
                }
                else if (key == "compression_level_brotli") {
                    int level = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), level);
                    if (result.ec == std::errc() && result.ptr == val.data() + val.size()) {
                        if (level >= 1 && level <= 11) {
                            override.compression_level_brotli = level;
                        } else {
                            Logger::warning(std::format("Invalid brotli_level {} in folder_override, must be 1-11", level));
                        }
                    } else {
                        Logger::warning("Invalid compression_level_brotli format in folder_override");
                    }
                }
                else if (key == "extensions") {
                    auto exts = split(val, ' ');
                    override.extensions.insert(override.extensions.end(), exts.begin(), exts.end());
                    // Нормализация расширений (lowercase)
                    for (auto& ext : override.extensions) {
                        std::transform(ext.begin(), ext.end(), ext.begin(),
                                       [](unsigned char c){ return ::tolower(c); });
                    }
                }
                else if (key == "process_files_without_extensions") {
                    override.process_files_without_extensions = (val == "true");
                }
                else if (key == "recursive") {
                    override.recursive = (val == "true");
                }
                else if (key == "rate_limit") {
                    int limit = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), limit);
                    if (result.ec == std::errc() && result.ptr == val.data() + val.size()) {
                        if (limit >= 0) {
                            override.rate_limit = limit;
                        }
                    } else {
                        Logger::warning("Invalid rate_limit format in folder_override");
                    }
                }
                else if (key == "io_delay_us") {
                    int delay = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), delay);
                    if (result.ec == std::errc() && result.ptr == val.data() + val.size()) {
                        if (delay >= 0 && delay <= 1000000) {
                            override.io_delay_us = delay;
                        } else {
                            Logger::warning(std::format("Invalid io_delay_us {} in folder_override", delay));
                        }
                    } else {
                        Logger::warning("Invalid io_delay_us format in folder_override");
                    }
                }
                else if (key == "max_active_ios") {
                    unsigned long long value = 0;
                    auto result = std::from_chars(val.data(), val.data() + val.size(), value);
                    if (result.ec == std::errc() && result.ptr == val.data() + val.size()) {
                        if (value <= 10000) {
                            override.max_active_ios = value;
                        } else {
                            Logger::warning(std::format("max_active_ios {} too high in folder_override, limiting to 10000", value));
                            override.max_active_ios = 10000;
                        }
                    } else {
                        Logger::warning("Invalid max_active_ios format in folder_override");
                    }
                }
            }
        }
    }

    // Defaults
    if (cfg.extensions.empty()) {
        cfg.extensions = {"txt", "js", "css", "svg", "json", "html", "htm", "map"};
    }
    if (cfg.target_paths.empty() && cfg.cli_dirs.empty()) {
        cfg.target_paths = {"/var/www/html"};
    }

    // CLI Overrides
    if (!cfg.cli_dirs.empty()) cfg.target_paths = cfg.cli_dirs;
    if (!cfg.cli_exts.empty()) cfg.extensions = cfg.cli_exts;
    if (cfg.cli_gzip_level != -1) cfg.gzip_level = cfg.cli_gzip_level;
    if (cfg.cli_brotli_level != -1) cfg.brotli_level = cfg.cli_brotli_level;

    // Normalize extensions (lowercase)
    for (auto& ext : cfg.extensions) {
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return ::tolower(c); });
    }

    return cfg;
}
