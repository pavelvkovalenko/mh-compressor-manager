#pragma once

/**
 * Утилиты оптимизации производительности
 *
 * Функции:
 * - CPU Affinity для потоков
 */
class PerformanceOptimizer {
public:
    /**
     * Настройка CPU Affinity для текущего потока
     * @param core_id ID ядра процессора
     * @return true при успехе
     */
    static bool set_cpu_affinity(int core_id);

    /**
     * Получение количества доступных ядер CPU
     * @return Количество ядер
     */
    static int get_cpu_count();
};
