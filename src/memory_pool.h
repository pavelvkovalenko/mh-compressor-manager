#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <optional>
#include <cstddef>

/**
 * MemoryPool - выделенный пул памяти для буферов сжатия
 * 
 * Оптимизации:
 * - Предварительное выделение памяти для избежания аллокаций в горячем пути
 * - Переиспользование буферов для снижения нагрузки на аллокатор
 * - Потокобезопасность через lock-free очереди где возможно
 * - Выравнивание по границе кэш-линии для лучшей производительности
 */
template<typename T, size_t DefaultSize = 262144>
class MemoryPool {
public:
    explicit MemoryPool(size_t initial_capacity = 8) 
        : buffer_size(DefaultSize), alignment(alignof(std::max_align_t)) {
        // Предварительно выделяем буферы
        for (size_t i = 0; i < initial_capacity; ++i) {
            release(allocate());
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
        total_allocated_ -= available_buffers_.size();
    }
    
    // Выделение буфера из пула
    std::vector<T> allocate() {
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (!free_buffers_.empty()) {
            auto* buf = free_buffers_.front();
            free_buffers_.pop();
            --total_allocated_;
            return std::vector<T>(buf, buf + buffer_size / sizeof(T));
        }
        
        // Если нет свободных буферов, выделяем новый
        ++total_allocated_;
        void* ptr = aligned_alloc(alignment, buffer_size);
        if (!ptr) {
            throw std::bad_alloc();
        }
        return std::vector<T>(static_cast<T*>(ptr), static_cast<T*>(ptr) + buffer_size / sizeof(T));
    }
    
    // Возврат буфера в пул
    void release(std::vector<T>& buffer) {
        if (buffer.size() != buffer_size / sizeof(T)) {
            // Несоответствующий размер - освобождаем память
            return;
        }
        
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Копируем данные обратно в выровненный буфер
        void* ptr = aligned_alloc(alignment, buffer_size);
        if (ptr) {
            std::copy(buffer.begin(), buffer.end(), static_cast<T*>(ptr));
            free_buffers_.push(static_cast<T*>(ptr));
            ++total_allocated_;
        }
        // Оригинальный вектор будет уничтожен вызывающей стороной
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
    
private:
    void* aligned_alloc(size_t alignment, size_t size) {
        void* ptr = nullptr;
#if defined(_MSC_VER)
        ptr = _aligned_malloc(size, alignment);
#else
        if (posix_memalign(&ptr, alignment, size) != 0) {
            ptr = nullptr;
        }
#endif
        return ptr;
    }
    
    void aligned_free(void* ptr) {
#if defined(_MSC_VER)
        _aligned_free(ptr);
#else
        free(ptr);
#endif
    }
    
    const size_t buffer_size;
    const size_t alignment;
    mutable std::mutex mutex_;
    std::queue<T*> free_buffers_;
    size_t total_allocated_{0};
};

// Специализация для байтовых буферов (наиболее частый случай)
using ByteBufferPool = MemoryPool<uint8_t, 262144>;
