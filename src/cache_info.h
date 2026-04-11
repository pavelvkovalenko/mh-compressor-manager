#pragma once
#include <cstddef>
#include <string>

/**
 * @brief Информация о кэш-иерархии CPU
 * Используется для адаптивного размера буфера сжатия (ТЗ §3.2.9)
 */
struct CacheInfo {
    size_t l1_dcache_per_core;   ///< L1 data cache на ядро (обычно 32-48 КБ)
    size_t l2_per_core;          ///< L2 cache на ядро (обычно 512 КБ – 2 МБ)
    size_t l3_total;             ///< L3 cache общий (12-480 МБ)
    size_t l3_shared_cores;      ///< Сколько ядер делят L3
    size_t thread_count;         ///< std::thread::hardware_concurrency()

    // Минимальный и максимальный размер буфера (ТЗ §3.2.9)
    static constexpr size_t MIN_BUFFER_SIZE = 64 * 1024;       // 64 КБ
    static constexpr size_t MAX_BUFFER_SIZE = 16 * 1024 * 1024; // 16 МБ
    static constexpr size_t FALLBACK_L3 = 2 * 1024 * 1024;      // 2 МБ fallback

    /**
     * @brief Оптимальный размер буфера на один поток
     * L3 / thread_count, clamp(MIN_BUFFER_SIZE, MAX_BUFFER_SIZE)
     */
    size_t optimal_buffer_size() const;

    /**
     * @brief Оптимальный размер чанка для streaming
     * То же что optimal_buffer_size()
     */
    size_t optimal_chunk_size() const;

    /**
     * @brief Определить параметры кэша системы
     * Основной метод: sysfs
     * Fallback: 2 МБ L3
     */
    static CacheInfo detect();

private:
    static size_t detect_l3_cache_size();
    static size_t parse_size_string(const std::string& str);
};
