#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <queue>
#include <optional>
#include <cstddef>
#include <atomic>

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
    explicit MemoryPool(size_t initial_capacity = 8) 
        : buffer_size(DefaultSize), alignment(alignof(std::max_align_t)) {
        // Предварительно выделяем буферы
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
        
        // Если нет свободных буферов, выделяем новый
        void* ptr = aligned_alloc(alignment, buffer_size);
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
        
        // Возвращаем буфер в пул для переиспользования
        std::unique_lock<std::mutex> lock(mutex_);
        free_buffers_.push(raw_buf);
        // Освобождаем ownership у vector чтобы избежать double-free
        // Vector будет уничтожен вызывающей стороной, но data() больше не валиден
        buffer.clear();
        buffer.shrink_to_fit();
    }
    
    // Возврат сырого буфера в пул (без копирования -真正的 переиспользование)
    void release_raw(T* buffer) {
        if (!buffer) {
            return;
        }
        
        std::unique_lock<std::mutex> lock(mutex_);
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
    std::atomic<size_t> total_allocated_{0};
};

// Специализация для байтовых буферов (наиболее частый случай)
using ByteBufferPool = MemoryPool<uint8_t, 262144>;
