#pragma once
#include <string>
#include <vector>
#include <map>

struct Config {
    std::vector<std::string> target_paths;
    bool debug = false;
    int threads = 0; // 0 = auto
    std::vector<std::string> extensions;
    std::string algorithms = "all"; // gzip, brotli, all
    int gzip_level = 6;
    int brotli_level = 4;
    int debounce_delay = 2; // seconds
    std::string config_path = "/etc/mediahive/compressor-manager.conf";
    
    // Ограничение I/O нагрузки
    int io_delay_us = 0;         // Задержка между файлами (микросекунды)
    size_t max_active_ios = 0;   // Лимит параллельных I/O операций (0 = без лимита)
    
    // CLI overrides
    std::vector<std::string> cli_dirs;
    std::vector<std::string> cli_exts;
    int cli_gzip_level = -1;
    int cli_brotli_level = -1;
    bool dry_run = false;
};

Config load_config(int argc, char* argv[]);
