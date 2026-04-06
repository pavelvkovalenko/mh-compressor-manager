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

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

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

Config load_config(int argc, char* argv[]) {
    Config cfg;
    
    // Simple CLI parsing
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: mh-compressor-manager [options]\n"
                      << "  --config <path>   Config file path\n"
                      << "  --dir <path>      Target directory (override)\n"
                      << "  --ext <list>      Extensions (override)\n"
                      << "  --gzip-level <N>  Gzip level\n"
                      << "  --brotli-level <N> Brotli level\n"
                      << "  --dry-run         Dry run mode\n"
                      << "  --version         Show version\n";
            exit(0);
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "mh-compressor-manager v1.0.0\n";
            exit(0);
        }
        if (arg == "--config" && i + 1 < argc) cfg.config_path = argv[++i];
        else if (arg == "--dir" && i + 1 < argc) cfg.cli_dirs.push_back(argv[++i]);
        else if (arg == "--ext" && i + 1 < argc) {
            auto exts = split(argv[++i], ' ');
            cfg.cli_exts.insert(cfg.cli_exts.end(), exts.begin(), exts.end());
        }
        else if (arg == "--gzip-level" && i + 1 < argc) cfg.cli_gzip_level = std::stoi(argv[++i]);
        else if (arg == "--brotli-level" && i + 1 < argc) cfg.cli_brotli_level = std::stoi(argv[++i]);
        else if (arg == "--dry-run") cfg.dry_run = true;
    }

    // Load INI
    std::ifstream file(cfg.config_path);
    if (!file.is_open()) {
        Logger::warning(std::format("Config file {} not found, using defaults", cfg.config_path));
    } else {
        std::string line;
        std::string current_section = "general";
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                current_section = line.substr(1, line.size() - 2);
                continue;
            }
            if (current_section == "general" || current_section.empty()) {
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string key = trim(line.substr(0, eq));
                    std::string val = trim(line.substr(eq + 1));
                    if (key == "target_path") cfg.target_paths = split(val, ';');
                    else if (key == "debug") cfg.debug = (val == "true");
                    else if (key == "threads") cfg.threads = std::stoi(val);
                    else if (key == "list") {
                        auto exts = split(val, ' ');
                        cfg.extensions.insert(cfg.extensions.end(), exts.begin(), exts.end());
                    }
                    else if (key == "algorithms") cfg.algorithms = val;
                    else if (key == "gzip_level") cfg.gzip_level = std::stoi(val);
                    else if (key == "brotli_level") cfg.brotli_level = std::stoi(val);
                    else if (key == "debounce_delay") cfg.debounce_delay = std::stoi(val);
                    else if (key == "io_delay_us") cfg.io_delay_us = std::stoi(val);
                    else if (key == "max_active_ios") cfg.max_active_ios = std::stoull(val);
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
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    return cfg;
}
