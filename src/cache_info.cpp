#include "cache_info.h"
#include "logger.h"
#include "i18n.h"
#include <fstream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

size_t CacheInfo::optimal_buffer_size() const {
    if (l3_total == 0 || thread_count == 0) {
        return FALLBACK_L3;  // Fallback при неизвестном L3 — 2 МБ по ТЗ §3.2.9
    }
    size_t per_thread = l3_total / thread_count;
    return std::clamp(per_thread, MIN_BUFFER_SIZE, MAX_BUFFER_SIZE);
}

size_t CacheInfo::optimal_chunk_size() const {
    return optimal_buffer_size();
}

/**
 * @brief Парсит строку размера из sysfs (формат: "33554432" или "32M" или "32G")
 */
size_t CacheInfo::parse_size_string(const std::string& str) {
    if (str.empty()) return 0;

    // Проверяем суффиксы M, G, K
    char suffix = str.back();
    std::string num_str = str;

    size_t multiplier = 1;
    if (suffix == 'M' || suffix == 'm') {
        multiplier = 1024 * 1024;
        num_str.pop_back();
    } else if (suffix == 'G' || suffix == 'g') {
        multiplier = 1024 * 1024 * 1024;
        num_str.pop_back();
    } else if (suffix == 'K' || suffix == 'k') {
        multiplier = 1024;
        num_str.pop_back();
    }

    try {
        size_t value = std::stoull(num_str);
        return value * multiplier;
    } catch (const std::exception& e) {
        Logger::warning(_("Failed to parse cache size string '%s': %s"), str.c_str(), e.what());
        return 0;
    }
}

/**
 * @brief Читает атрибут из sysfs
 */
static std::string read_sysfs_attr(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string line;
    std::getline(f, line);
    // Убираем trailing whitespace
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

size_t CacheInfo::detect_l3_cache_size() {
    const std::string cpu_base = "/sys/devices/system/cpu/cpu0/cache/";

    if (!fs::exists(cpu_base)) {
        Logger::warning(_("sysfs cache info not available, using fallback L3 = 2 MB"));
        return FALLBACK_L3;
    }

    size_t max_l3 = 0;

    // Перебираем все index<N>/ директории
    try {
        for (const auto& entry : fs::directory_iterator(cpu_base)) {
            if (!entry.is_directory()) continue;

            std::string dirname = entry.path().filename().string();
            // Формат: index0, index1, index2, ...
            if (dirname.rfind("index", 0) != 0) continue;

            std::string base = entry.path().string() + "/";

            // Читаем level
            std::string level_str = read_sysfs_attr(base + "level");
            if (level_str.empty()) continue;

            int level = 0;
            try {
                level = std::stoi(level_str);
            } catch (...) {
                continue;
            }

            // Ищем только L3
            if (level != 3) continue;

            // Проверяем type = Unified
            std::string type = read_sysfs_attr(base + "type");
            if (type != "Unified") continue;

            // Читаем size
            std::string size_str = read_sysfs_attr(base + "size");
            if (size_str.empty()) continue;

            size_t size = parse_size_string(size_str);
            if (size > max_l3) {
                max_l3 = size;
            }
        }
    } catch (const std::exception& e) {
        Logger::warning(_("Error reading sysfs cache info: %s"), e.what());
    }

    if (max_l3 == 0) {
        Logger::warning(_("Failed to determine CPU cache size (sysfs unavailable), using fallback: L3 = 2 MB"));
        return FALLBACK_L3;
    }

    return max_l3;
}

CacheInfo CacheInfo::detect() {
    CacheInfo info = {};

    // Определяем количество потоков
    info.thread_count = std::thread::hardware_concurrency();
    if (info.thread_count == 0) {
        info.thread_count = 4;  // Fallback
    }

    // Определяем L3
    info.l3_total = detect_l3_cache_size();

    // L1 dcache и L2 — читаем из cpu0
    const std::string cpu_base = "/sys/devices/system/cpu/cpu0/cache/";
    if (fs::exists(cpu_base)) {
        try {
            for (const auto& entry : fs::directory_iterator(cpu_base)) {
                if (!entry.is_directory()) continue;
                std::string base = entry.path().string() + "/";

                std::string level_str = read_sysfs_attr(base + "level");
                std::string type = read_sysfs_attr(base + "type");
                std::string size_str = read_sysfs_attr(base + "size");

                if (level_str.empty() || size_str.empty()) continue;

                int level = 0;
                try { level = std::stoi(level_str); } catch (...) { continue; }

                size_t size = parse_size_string(size_str);

                if (level == 1 && type == "Data") {
                    info.l1_dcache_per_core = size;
                } else if (level == 2 && type == "Unified") {
                    info.l2_per_core = size;
                }
            }
        } catch (...) {
            // Игнорируем ошибки чтения L1/L2
        }
    }

    // L3 shared cores — читаем из shared_cpu_list
    // Для простоты считаем что L3 общий для всех ядер (типично для consumer CPU)
    info.l3_shared_cores = info.thread_count;

    // Логирование
    if (info.l1_dcache_per_core > 0) {
        Logger::info(_("CPU cache: L1d = %zu KB/core, L2 = %zu KB/core, L3 = %zu MB"),
                                  info.l1_dcache_per_core / 1024,
                                  info.l2_per_core / 1024,
                                  info.l3_total / (1024 * 1024));
    } else {
        Logger::info(_("CPU cache: L3 = %zu MB (L1/L2 undefined)"),
                                  info.l3_total / (1024 * 1024));
    }

    Logger::info(_("Optimal buffer per thread: %zu KB (threads: %zu)"),
                              info.optimal_buffer_size() / 1024,
                              info.thread_count);

    return info;
}
