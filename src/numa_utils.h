#ifndef NUMA_UTILS_H
#define NUMA_UTILS_H

#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

/**
 * @brief Утилиты для NUMA-оптимизации
 * 
 * Предоставляет функции для определения NUMA-узла файла,
 * привязки потоков к узлам и выделения памяти с учетом NUMA.
 */
class NumaUtils {
public:
    /**
     * @brief Инициализация NUMA (вызвать один раз при старте)
     * 
     * Проверяет доступность NUMA в системе.
     * @return true если NUMA доступен
     */
    static bool initialize();
    
    /**
     * @brief Проверка, доступен ли NUMA
     */
    static bool is_numa_available() { return numa_available_; }
    
    /**
     * @brief Получить количество NUMA узлов
     */
    static int get_numa_node_count();
    
    /**
     * @brief Определить NUMA узел для файла
     * 
     * Определяет, к какому NUMA узлу ближе всего контроллер диска,
     * на котором расположен файл.
     * 
     * @param path Путь к файлу
     * @return Номер NUMA узла или -1 если не удалось определить
     */
    static int get_file_numa_node(const fs::path& path);
    
    /**
     * @brief Привязать текущий поток к NUMA узлу
     * 
     * @param node_id Номер NUMA узла
     * @return true если успешно
     */
    static bool bind_current_thread_to_node(int node_id);
    
    /**
     * @brief Выделить память с привязкой к NUMA узлу
     * 
     * @param size Размер памяти в байтах
     * @param node_id Номер NUMA узла
     * @return Указатель на выделенную память или nullptr при ошибке
     */
    static void* allocate_on_node(size_t size, int node_id);
    
    /**
     * @brief Освободить память, выделенную через allocate_on_node
     * 
     * @param ptr Указатель на память
     * @param size Размер памяти
     */
    static void free_on_node(void* ptr, size_t size);
    
    /**
     * @brief Привязать память к NUMA узлу
     * 
     * Использует mbind() для привязки существующего диапазона памяти
     * к указанному NUMA узлу.
     * 
     * @param ptr Указатель на память
     * @param size Размер памяти
     * @param node_id Номер NUMA узла
     * @return true если успешно
     */
    static bool bind_memory_to_node(void* ptr, size_t size, int node_id);
    
    /**
     * @brief Получить оптимальный NUMA узел для диска
     * 
     * Определяет оптимальный NUMA узел для устройства по пути.
     * 
     * @param device_path Путь к устройству или файлу
     * @return Номер NUMA узла или 0 по умолчанию
     */
    static int get_optimal_node_for_device(const fs::path& device_path);

private:
    static bool numa_available_;
    static int numa_node_count_;
};

#endif // NUMA_UTILS_H
