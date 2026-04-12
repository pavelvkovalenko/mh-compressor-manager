#include "numa_utils.h"

#if HAVE_NUMA
#include <numa.h>
#include <numaif.h>
#endif
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/mman.h>  // Для mbind
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif

// Используем префикс NUMA_ чтобы избежать конфликта с syslog.h
#define NUMA_LOG_INFO(msg, ...) Logger::info(std::format(msg, ##__VA_ARGS__))
#define NUMA_LOG_ERROR(msg, ...) Logger::error(std::format(msg, ##__VA_ARGS__))
#define NUMA_LOG_WARN(msg, ...) Logger::warning(std::format(msg, ##__VA_ARGS__))
#define NUMA_LOG_DEBUG(msg, ...) Logger::debug(std::format(msg, ##__VA_ARGS__))

#include "logger.h"

// Статические члены
bool NumaUtils::numa_available_ = false;
int NumaUtils::numa_node_count_ = 1;

/**
 * @brief Кэш соответствия устройств и NUMA узлов
 */
static std::map<std::string, int> device_numa_cache;
static std::mutex device_numa_cache_mutex;

bool NumaUtils::initialize() {
#if HAVE_NUMA
    if (numa_available() == 0) {
        numa_available_ = true;
        numa_node_count_ = numa_num_configured_nodes();
        NUMA_LOG_INFO("NUMA доступен: {} узлов", numa_node_count_);

        // Инициализация политик NUMA
        numa_set_interleave_mask(numa_all_nodes_ptr);
        return true;
    } else {
        numa_available_ = false;
        numa_node_count_ = 1;
        NUMA_LOG_INFO("NUMA не доступен в этой системе");
        return false;
    }
#else
    numa_available_ = false;
    numa_node_count_ = 1;
    NUMA_LOG_INFO("NUMA support disabled (libnuma not found at compile time)");
    return false;
#endif
}

int NumaUtils::get_numa_node_count() {
    return numa_node_count_;
}

int NumaUtils::get_file_numa_node(const fs::path& path) {
    if (!numa_available_) {
        return 0;
    }
    
    try {
        // Получаем информацию о файле
        struct stat st;
        if (stat(path.c_str(), &st) < 0) {
            NUMA_LOG_WARN("Не удалось получить stat для {}: {}", path.string(), strerror(errno));
            return 0;
        }
        
        // Определяем устройство
        dev_t dev = st.st_dev;
        
        // Проверяем кэш
        std::string dev_key = std::to_string(major(dev)) + ":" + std::to_string(minor(dev));
        {
            std::lock_guard<std::mutex> lock(device_numa_cache_mutex);
            auto it = device_numa_cache.find(dev_key);
            if (it != device_numa_cache.end()) {
                return it->second;
            }
        }

        // Попытка определить NUMA узел через sysfs
        int node = get_optimal_node_for_device(path);
        {
            std::lock_guard<std::mutex> lock(device_numa_cache_mutex);
            device_numa_cache[dev_key] = node;
        }
        
        return node;
    } catch (const std::exception& e) {
        NUMA_LOG_WARN("Ошибка определения NUMA узла для {}: {}", path.string(), e.what());
        return 0;
    }
}

bool NumaUtils::bind_current_thread_to_node(int node_id) {
#if HAVE_NUMA
    if (!numa_available_ || node_id < 0 || node_id >= numa_node_count_) {
        return false;
    }
    
    struct bitmask* nodemask = numa_bitmask_alloc(numa_num_possible_nodes());
    if (!nodemask) {
        return false;
    }
    
    numa_bitmask_setbit(nodemask, node_id);
    
    // Привязка потока к узлу
    if (numa_run_on_node(node_id) == 0) {
        numa_bitmask_free(nodemask);
        NUMA_LOG_DEBUG("Поток привязан к NUMA узлу {}", node_id);
        return true;
    }
    
    numa_bitmask_free(nodemask);
    NUMA_LOG_WARN("Не удалось привязать поток к NUMA узлу {}", node_id);
    return false;
#else
    (void)node_id;
    return false;
#endif
}

void* NumaUtils::allocate_on_node(size_t size, int node_id) {
#if HAVE_NUMA
    if (!numa_available_) {
        return malloc(size);
    }
    
    if (node_id < 0 || node_id >= numa_node_count_) {
        node_id = 0;
    }
    
    void* ptr = numa_alloc_onnode(size, node_id);
    if (!ptr) {
        NUMA_LOG_ERROR("Не удалось выделить память на NUMA узле {}: {}", node_id, strerror(errno));
        return nullptr;
    }
    
    NUMA_LOG_DEBUG("Выделено {} байт на NUMA узле {}", size, node_id);
    return ptr;
#else
    (void)node_id;
    (void)size;
    return malloc(size);
#endif
}

void NumaUtils::free_on_node(void* ptr, size_t size) {
#if HAVE_NUMA
    if (!numa_available_ || !ptr) {
        free(ptr);
        return;
    }

    numa_free(ptr, size);
#else
    (void)size;
    free(ptr);
#endif
}

bool NumaUtils::bind_memory_to_node(void* ptr, size_t size, int node_id) {
#if HAVE_NUMA
    if (!numa_available_ || !ptr) {
        return false;
    }
    
    if (node_id < 0 || node_id >= numa_node_count_) {
        node_id = 0;
    }
    
    struct bitmask* nodemask = numa_bitmask_alloc(numa_num_possible_nodes());
    if (!nodemask) {
        return false;
    }
    
    numa_bitmask_setbit(nodemask, node_id);
    
    // Используем mbind для привязки страниц
    unsigned long flags = MPOL_MF_MOVE | MPOL_MF_STRICT;
    if (mbind(ptr, size, MPOL_BIND, nodemask->maskp, nodemask->size, flags) == 0) {
        numa_bitmask_free(nodemask);
        NUMA_LOG_DEBUG("Память привязана к NUMA узлу {}", node_id);
        return true;
    }
    
    numa_bitmask_free(nodemask);
    NUMA_LOG_WARN("Не удалось привязать память к NUMA узлу {}: {}", node_id, strerror(errno));
    return false;
#else
    (void)ptr;
    (void)size;
    (void)node_id;
    return false;
#endif
}

int NumaUtils::get_optimal_node_for_device(const fs::path& device_path) {
#if HAVE_NUMA
    if (!numa_available_) {
        return 0;
    }

    try {
        // Получаем канонический путь
        fs::path canonical = fs::canonical(device_path);

        // Пытаемся определить устройство через stat
        struct stat st;
        if (stat(canonical.c_str(), &st) < 0) {
            return 0;
        }

        // Для простоты возвращаем узел 0
        // В реальной реализации можно парсить /sys/block/*/device/numa_node
        NUMA_LOG_DEBUG("Оптимальный NUMA узел для {}: 0 (по умолчанию)", device_path.string());
        return 0;

    } catch (const std::exception& e) {
        NUMA_LOG_WARN("Ошибка определения устройства для {}: {}", device_path.string(), e.what());
        return 0;
    }
#else
    (void)device_path;
    return 0;
#endif
}
