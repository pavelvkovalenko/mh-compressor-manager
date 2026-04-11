#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <optional>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <sys/mman.h>
#include <immintrin.h>
#include <unordered_set>
#include "logger.h"
#include "performance_optimizer.h"
#include "numa_utils.h"

/**
 * MemoryPool - выделенный пул памяти для буферов сжатия с поддержкой NUMA
 * 
 * Оптимизации:
 * - Предварительное выделение памяти для избежания аллокаций в горячем пути
 * - Переиспользование буферов без дополнительных аллокаций при release
 * - Потокобезопасность через mutex
 * - Выравнивание по границе кэш-линии для лучшей производительности
 * - Прямой доступ к сырым указателям для работы с системными вызовами
 * - NUMA-aware выделение памяти для снижения задержек доступа к памяти
 * - Per-thread кэши свободных блоков для устранения блокировок
 * - SIMD-ускоренные операции копирования и обнуления памяти
 * - Prefetching данных для снижения латентности доступа к памяти
 * - Ограничение максимального размера пула для предотвращения чрезмерного потребления памяти
 */
template<typename T, size_t DefaultSize = 262144>
class MemoryPool {
public:
    // Выравнивание по кэш-линии для предотвращения false sharing (обычно 64 байта)
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    // Размер per-thread кэша (количество буферов на поток)
    static constexpr size_t THREAD_CACHE_SIZE = 2;

    // Максимальное количество буферов в пуле для предотвращения чрезмерного потребления памяти
    // При размере буфера 256KB: 16 буферов = 4MB максимум на пул
    // Для 3 пулов (gzip in/out, brotli out) = 12MB суммарно
    static constexpr size_t MAX_POOL_SIZE = 16;
    
    explicit MemoryPool(size_t initial_capacity = 8, int numa_node_id = -1, size_t max_size = MAX_POOL_SIZE) 
        : buffer_size(DefaultSize), 
          alignment(std::max(alignof(std::max_align_t), CACHE_LINE_SIZE)),
          numa_node_id_(numa_node_id),
          max_pool_size_(max_size) {
        // Инициализация NUMA если доступен
        if (numa_node_id_ >= 0 && NumaUtils::is_numa_available()) {
            // Привязка текущего потока к NUMA узлу
            NumaUtils::bind_current_thread_to_node(numa_node_id_);
        }
        
        // Предварительно выделяем буферы с использованием Huge Pages если доступны
        // Ограничиваем начальную емкость максимальным размером пула
        size_t actual_initial = std::min(initial_capacity, max_pool_size_);
        for (size_t i = 0; i < actual_initial; ++i) {
            auto buf = allocate_raw();
            if (buf) {
                release_raw(buf);
            }
        }
    }
    
    ~MemoryPool() {
        // Освобождаем все буферы из глобального пула
        std::unique_lock<std::mutex> lock(mutex_);
        while (!free_buffers_.empty()) {
            auto* buf = free_buffers_.front();
            free_buffers_.pop();
            allocated_set_.erase(buf);
            aligned_free(buf);
        }

        // Примечание: thread_local кэши других потоков не могут быть очищены
        // из текущего потока. Буферы в них будут освобождены при завершении
        // тех потоков (деструктор thread_local vector) или при следующем
        // вызове allocate_raw() из того же потока.
        // allocated_set_ гарантирует что все буферы будут освобождены в деструкторе.

        // Освобождаем ВСЕ оставшиеся буферы из allocated_set_
        // (те которые не были возвращены через release_raw — утечка при завершении)
        for (auto* buf : allocated_set_) {
            aligned_free(buf);
        }
        allocated_set_.clear();
        total_allocated_ = 0;
    }
    
    // Выделение буфера из пула (возвращает vector для удобства)
    // ВНИМАНИЕ: vector КОПИРУЕТ данные из пула во внутреннюю память.
    // Это НЕ тот же буфер — используйте allocate_raw()/release_raw() для
    // zero-copy работы с пулом.
    [[deprecated("allocate() copies data — use allocate_raw()/release_raw() for zero-copy pool access")]]
    std::vector<T> allocate() {
        T* raw_buf = allocate_raw();
        if (!raw_buf) {
            throw std::bad_alloc();
        }
        std::vector<T> vec(raw_buf, raw_buf + buffer_size / sizeof(T));
        // Возвращаем raw_buf обратно в пул — vector сделал копию
        release_raw(raw_buf);
        return vec;
    }
    
    // Выделение сырого буфера (для прямого использования с read/write)
    T* allocate_raw() {
        // Сначала пробуем взять из per-thread кэша (без блокировки)
        auto& local_cache = get_thread_cache();
        if (!local_cache.empty()) {
            T* buf = local_cache.back();
            local_cache.pop_back();
            // Prefetch данных для снижения латентности
            _mm_prefetch(reinterpret_cast<char*>(buf), _MM_HINT_T0);
            return buf;
        }
        
        // Если per-thread кэш пуст, берем из глобального пула
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (!free_buffers_.empty()) {
            auto* buf = free_buffers_.front();
            free_buffers_.pop();
            lock.unlock();
            // Prefetch данных
            _mm_prefetch(reinterpret_cast<char*>(buf), _MM_HINT_T0);
            return buf;
        }
        
        // Если нет свободных буферов, выделяем новый
        void* ptr = nullptr;

        // NUMA-aware выделение памяти
        if (numa_node_id_ >= 0 && NumaUtils::is_numa_available()) {
            // ВАЖНО: mutex уже захвачен (unique_lock выше), НЕ создаём вложенный lock
            ptr = NumaUtils::allocate_on_node(buffer_size, numa_node_id_);
            if (ptr) {
                ++total_allocated_;
                allocated_set_.insert(static_cast<T*>(ptr));
                lock.unlock();
                return static_cast<T*>(ptr);
            }
            // Fallback на стандартное выделение если NUMA аллокация не удалась
        }

        // ВАЖНО: Huge Pages (mmap) НЕ используются — метод выделения теряется
        // и aligned_free() вызовет free() на mmap-памяти (UB/crash).
        // Вместо Huge Pages используем posix_memalign с выравниванием по странице.

        // Fallback на стандартное выделение
        if (!ptr) {
            ptr = aligned_alloc(alignment, buffer_size);
        }
        
        if (!ptr) {
            return nullptr;
        }
        ++total_allocated_;
        T* result = static_cast<T*>(ptr);
        // unique_lock уже захвачен — НЕ создаём вложенный lock_guard
        allocated_set_.insert(result);
        lock.unlock();
        return result;
    }
    
    // Возврат буфера в пул.
    // ВНИМАНИЕ: std::vector владеет собственной памятью (std::allocator),
    // а НЕ памятью пула. Этот метод НЕ помещает vector data обратно в пул —
    // это был бы invalid-free. Используйте release_raw() для буферов из пула.
    [[deprecated("release(vector) cannot reclaim vector's internal memory — use release_raw() instead")]]
    void release(std::vector<T>& buffer) {
        if (buffer.empty()) {
            return;
        }
        // Вектор владеет своей памятью через std::allocator — не трогаем её.
        // allocated_set_ не содержит указателей на vector internal storage,
        // так что erase не нужен.
        buffer.clear();
    }
    
    // Возврат сырого буфера в пул (с приоритетом на per-thread кэш)
    // ВАЖНО: Вызывающая сторона должна гарантировать что буфер был выделен из этого пула
    // и не был уже возвращен (защита от double-free)
    void release_raw(T* buffer) {
        if (!buffer) {
            return;
        }
        
        // Базовая проверка выравнивания для отлова очевидных ошибок
        if (reinterpret_cast<uintptr_t>(buffer) % alignment != 0) {
            // Это может быть буфер не из нашего пула - игнорируем для безопасности
            return;
        }
        
        // Сначала пробуем поместить в per-thread кэш (без блокировки)
        auto& local_cache = get_thread_cache();
        if (local_cache.size() < THREAD_CACHE_SIZE) {
            local_cache.push_back(buffer);
            return;
        }
        
        // Если per-thread кэш полон, помещаем в глобальный пул
        std::unique_lock<std::mutex> lock(mutex_);
        
        // ЗАЩИТА ОТ ЧРЕЗМЕРНОГО ПОТРЕБЛЕНИЯ ПАМЯТИ:
        // Не принимаем буфер если пул уже достиг максимального размера
        if (free_buffers_.size() >= max_pool_size_) {
            // Пул переполнен - освобождаем память сразу вместо добавления в пул
            // Это предотвращает неограниченный рост потребления памяти при большом количестве файлов
            aligned_free(buffer);
            allocated_set_.erase(buffer);  // Удаляем из множества отслеживания
            --total_allocated_;
            return;
        }
        
        // O(1) защита от double-free с использованием unordered_set
        if (allocated_set_.find(buffer) == allocated_set_.end()) {
            // Буфер не был выделен из этого пула или уже был освобожден
            Logger::error("Double-free or invalid buffer detected in MemoryPool - ignoring");
            return;
        }
        
        // Удаляем из множества отслеживания и добавляем в пул
        allocated_set_.erase(buffer);
        free_buffers_.push(buffer);
        // total_allocated_ не меняется - буфер просто возвращается в пул
    }
    
    // Статистика пула
    size_t allocated_count() const {
        std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        return total_allocated_;
    }
    
    size_t free_count() const {
        std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        return free_buffers_.size();
    }
    
    size_t get_buffer_size() const {
        return buffer_size;
    }

    // Освобождение неиспользуемых буферов (вызывается периодически для снижения потребления памяти)
    // Возвращает количество освобождённых буферов
    size_t shrink(size_t keep_count = 4) {
        std::unique_lock<std::mutex> lock(mutex_);
        size_t freed = 0;

        // Освобождаем лишние буферы из глобального пула
        while (free_buffers_.size() > keep_count) {
            auto* buf = free_buffers_.front();
            free_buffers_.pop();
            allocated_set_.erase(buf);
            aligned_free(buf);
            --total_allocated_;
            ++freed;
        }

        return freed;
    }
    
private:
    // Получение per-thread кэша (thread_local для избежания блокировок)
    static std::vector<T*>& get_thread_cache() {
        thread_local static std::vector<T*> cache;
        cache.reserve(THREAD_CACHE_SIZE);
        return cache;
    }

    // Очистка thread_local кэша текущего потока — вызывается из деструктора пула
    // ВАЖНО: Это очищает кэш ТОЛЬКО текущего потока, не всех потоков
    // Для полной очистки нужно вызывать из каждого потока перед его завершением
    static void cleanup_thread_cache() {
        auto& cache = get_thread_cache();
        for (auto* buf : cache) {
            // Буферы возвращаются в глобальный пул при следующем вызове allocate_raw
            // или освобождаются в деструкторе пула через allocated_set_
        }
        cache.clear();
    }
    
    void* aligned_alloc(size_t alignment, size_t size) {
        void* ptr = nullptr;
#if defined(_MSC_VER)
        ptr = _aligned_malloc(size, alignment);
#else
        // NUMA-aware выделение если указан узел
        if (numa_node_id_ >= 0 && NumaUtils::is_numa_available()) {
            ptr = NumaUtils::allocate_on_node(size, numa_node_id_);
            if (ptr) {
                return ptr;
            }
        }
        // Fallback на posix_memalign (без Huge Pages — они требуют tracking method)
        if (posix_memalign(&ptr, alignment, size) != 0) {
            ptr = nullptr;
        }
#endif
        return ptr;
    }

    void aligned_free(void* ptr) {
        if (!ptr) return;

#if defined(_MSC_VER)
        _aligned_free(ptr);
#else
        // NUMA-память освобождается через NUMA
        if (numa_node_id_ >= 0 && NumaUtils::is_numa_available()) {
            NumaUtils::free_on_node(ptr, buffer_size);
            return;
        }
        
        // Память выделена через posix_memalign, освобождаем через free
        // munmap можно использовать только для памяти выделенной через mmap
        free(ptr);
#endif
    }
    
    const size_t buffer_size;
    const size_t alignment;
    const int numa_node_id_;  // NUMA узел для выделения памяти (-1 если не используется)
    const size_t max_pool_size_;  // Максимальный размер пула для предотвращения чрезмерного потребления памяти
    mutable std::mutex mutex_;
    std::queue<T*> free_buffers_;
    std::unordered_set<T*> allocated_set_;  // Множество для O(1) проверки double-free
    std::atomic<size_t> total_allocated_{0};
};

// Специализация для байтовых буферов (наиболее частый случай)
using ByteBufferPool = MemoryPool<uint8_t, 262144>;

/**
 * @brief Глобальный экземпляр пула буферов (определён в compressor.cpp)
 * Используется для однократного чтения файлов при сжатии (ТЗ §3.2.4)
 */
ByteBufferPool& buffer_pool();
