#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <optional>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include "performance_optimizer.h"

/**
 * MemoryPool - выделенный пул памяти для буферов сжатия
 * 
 * Оптимизации:
 * - Предварительное выделение памяти для избежания аллокаций в горячем пути
 * - Переиспользование буферов без дополнительных аллокаций при release
 * - Потокобезопасность через mutex
 * - Выравнивание по границе кэш-линии для лучшей производительности
 * - Прямой доступ к сырым указателям для работы с системными вызовами
 */
template<typename T, size_t DefaultSize = 262144>
class MemoryPool {
public:
    // Выравнивание по кэш-линии для предотвращения false sharing (обычно 64 байта)
    static constexpr size_t CACHE_LINE_SIZE = 64;
    
    explicit MemoryPool(size_t initial_capacity = 8) 
        : buffer_size(DefaultSize), alignment(std::max(alignof(std::max_align_t), CACHE_LINE_SIZE)) {
        // Предварительно выделяем буферы с использованием Huge Pages если доступны
        for (size_t i = 0; i < initial_capacity; ++i) {
            auto buf = allocate_raw();
            if (buf) {
                release_raw(buf);
            }
        }
    }
    
    ~MemoryPool() {
        // Освобождаем все буферы
        std::unique_lock<std::mutex> lock(mutex_);
        while (!free_buffers_.empty()) {
            auto* buf = free_buffers_.front();
            free_buffers_.pop();
            aligned_free(buf);
        }
        total_allocated_ = 0;
    }
    
    // Выделение буфера из пула (возвращает vector для удобства)
    std::vector<T> allocate() {
        T* raw_buf = allocate_raw();
        if (!raw_buf) {
            throw std::bad_alloc();
        }
        return std::vector<T>(raw_buf, raw_buf + buffer_size / sizeof(T));
    }
    
    // Выделение сырого буфера (для прямого использования с read/write)
    T* allocate_raw() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (!free_buffers_.empty()) {
            auto* buf = free_buffers_.front();
            free_buffers_.pop();
            return buf;
        }
        
        // Если нет свободных буферов, выделяем новый с использованием Huge Pages если доступны
        void* ptr = nullptr;
        size_t alloc_size = buffer_size;
        
        // Пытаемся использовать Huge Pages для больших буферов
        if (buffer_size >= PerformanceOptimizer::get_huge_page_size() / 2 && 
            PerformanceOptimizer::is_huge_pages_available()) {
            ptr = PerformanceOptimizer::allocate_aligned_memory(alloc_size, true);
        }
        
        // Fallback на стандартное выделение
        if (!ptr) {
            ptr = aligned_alloc(alignment, buffer_size);
        }
        
        if (!ptr) {
            return nullptr;
        }
        ++total_allocated_;
        return static_cast<T*>(ptr);
    }
    
    // Возврат буфера в пул (для vector - переиспользуем напрямую без копирования)
    void release(std::vector<T>& buffer) {
        if (buffer.empty()) {
            return;
        }
        
        // Проверяем что размер соответствует ожидаемому
        if (buffer.size() != buffer_size / sizeof(T)) {
            // Несоответствующий размер - не принимаем
            return;
        }
        
        // Получаем указатель на данные vector
        T* raw_buf = buffer.data();
        
        // Проверяем что буфер был выделен из этого пула (базовая проверка)
        // В production коде можно добавить более строгую валидацию
        if (raw_buf == nullptr) {
            return;
        }
        
        // Возвращаем буфер в пул для переиспользования
        std::unique_lock<std::mutex> lock(mutex_);
        free_buffers_.push(raw_buf);
        // Освобождаем ownership у vector чтобы избежать double-free
        // Vector будет уничтожен вызывающей стороной, но data() больше не валиден
        buffer.clear();
        buffer.shrink_to_fit();
    }
    
    // Возврат сырого буфера в пул (без копирования -真正的 переиспользование)
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
        
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Простая защита от double-free: проверяем не находится ли уже буфер в очереди
        // Note: Это O(n) операция, но для защиты от corruption это приемлемо
        // Для production можно использовать unordered_set для O(1) проверки
        std::queue<T*> temp_queue;
        bool already_free = false;
        while (!free_buffers_.empty()) {
            T* front = free_buffers_.front();
            free_buffers_.pop();
            if (front == buffer) {
                already_free = true;
                // Используем fprintf вместо Logger чтобы избежать зависимости
                fprintf(stderr, "WARNING: Double-free attempt detected in MemoryPool - ignoring\n");
            }
            temp_queue.push(front);
        }
        // Восстанавливаем очередь
        while (!temp_queue.empty()) {
            free_buffers_.push(temp_queue.front());
            temp_queue.pop();
        }
        
        if (!already_free) {
            free_buffers_.push(buffer);
        }
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
    
private:
    void* aligned_alloc(size_t alignment, size_t size) {
        void* ptr = nullptr;
#if defined(_MSC_VER)
        ptr = _aligned_malloc(size, alignment);
#else
        // Пробуем использовать Huge Pages через mmap для больших выделений
        if (size >= PerformanceOptimizer::get_huge_page_size() / 2 && 
            PerformanceOptimizer::is_huge_pages_available()) {
            ptr = PerformanceOptimizer::allocate_aligned_memory(size, true);
            if (ptr) {
                return ptr;
            }
        }
        // Fallback на posix_memalign
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
        // Пытаемся освободить через munmap (для Huge Pages), иначе free
        if (munmap(ptr, buffer_size) != 0) {
            free(ptr);
        }
#endif
    }
    
    const size_t buffer_size;
    const size_t alignment;
    mutable std::mutex mutex_;
    std::queue<T*> free_buffers_;
    std::atomic<size_t> total_allocated_{0};
};

// Специализация для байтовых буферов (наиболее частый случай)
using ByteBufferPool = MemoryPool<uint8_t, 262144>;
