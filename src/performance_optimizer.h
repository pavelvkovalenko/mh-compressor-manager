#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <optional>
#include <thread>
#include <atomic>

namespace fs = std::filesystem;

/**
 * Оптимизированный класс для управления производительностью
 * 
 * Функции:
 * - Управление Huge Pages
 * - CPU Affinity для потоков
 * - Предварительное выделение памяти
 * - Оптимизация для SSD/NVMe
 */
class PerformanceOptimizer {
public:
    /**
     * Инициализация оптимизаций производительности
     * @param use_huge_pages Использовать Huge Pages (2MB страницы)
     * @param io_threads Количество потоков для I/O операций
     * @param cpu_threads Количество потоков для CPU операций
     */
    static void init(bool use_huge_pages = true, 
                     int io_threads = -1,
                     int cpu_threads = -1);
    
    /**
     * Настройка CPU Affinity для текущего потока
     * @param core_id ID ядра процессора
     * @return true при успехе
     */
    static bool set_cpu_affinity(int core_id);
    
    /**
     * Настройка CPU Affinity для потока
     * @param thread Ссылка на поток
     * @param core_id ID ядра процессора
     * @return true при успехе
     */
    static bool set_thread_cpu_affinity(std::thread& thread, int core_id);
    
    /**
     * Выделение выровненной памяти с поддержкой Huge Pages
     * @param size Размер в байтах
     * @param use_huge_page Использовать Huge Pages
     * @return Указатель на выделенную память или nullptr
     */
    // Выделение памяти с выравниванием. Возвращает {ptr, allocation_method}.
    // allocation_method: 0 = posix_memalign, 1 = mmap(Huge Pages), 2 = mmap(regular)
    struct AllocatedMemory { void* ptr; int method; size_t size; };
    static AllocatedMemory allocate_aligned_memory(size_t size, bool use_huge_page = false);
    static void free_aligned_memory(const AllocatedMemory& mem);
    
    /**
     * Освобождение выровненной памяти
     * @param ptr Указатель на память
     * @param size Размер выделенной памяти
     */
    static void free_aligned_memory_old(void* ptr, size_t size);
    
    /**
     * Освобождение выровненной памяти
     * @param ptr Указатель на память
     * @param size Размер выделенной памяти
     */
    static void free_aligned_memory(const AllocatedMemory& mem);
    
    /**
     * Предварительное выделение места на диске
     * @param fd Дескриптор файла
     * @param size Размер в байтах
     * @return true при успехе
     */
    static bool preallocate_file(int fd, uint64_t size);
    
    /**
     * Дает подсказки ядру о паттернах доступа к файлу
     * @param fd Дескриптор файла
     * @param sequential Последовательный доступ
     * @param no_reuse Не переиспользовать данные (для одноразового чтения)
     * @param write_once Запись один раз (для выходных файлов)
     * @return true при успехе
     */
    static bool advise_file_access(int fd, bool sequential = true, 
                                   bool no_reuse = true,
                                   bool write_once = false);
    
    /**
     * Получение количества доступных ядер CPU
     * @return Количество ядер
     */
    static int get_cpu_count();
    
    /**
     * Получение оптимального количества I/O потоков
     * @return Количество потоков
     */
    static int get_optimal_io_threads();
    
    /**
     * Получение оптимального количества CPU потоков
     * @return Количество потоков
     */
    static int get_optimal_cpu_threads();
    
    /**
     * Проверка поддержки Huge Pages в системе
     * @return true если доступны
     */
    static bool is_huge_pages_available();
    
    /**
     * Проверка поддержки io_uring
     * @return true если доступен
     */
    static bool is_io_uring_available();
    
    /**
     * Получение размера страницы памяти
     * @return Размер страницы в байтах
     */
    static size_t get_page_size();
    
    /**
     * Получение размера Huge Page
     * @return Размер Huge Page в байтах (обычно 2MB)
     */
    static size_t get_huge_page_size();

private:
    static bool huge_pages_initialized_;
    static int io_thread_count_;
    static int cpu_thread_count_;
    static size_t page_size_;
    static size_t huge_page_size_;
};
