#ifndef PIPELINE_H
#define PIPELINE_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <functional>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

/**
 * @brief Кольцевой буфер для передачи данных между этапами конвейера
 * 
 * Потокобезопасная реализация ring buffer с поддержкой блокирующей
 * записи и чтения. Используется для передачи блоков данных между
 * потоками чтения, сжатия и записи.
 * 
 * @tparam T Тип передаваемых данных
 */
template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) 
        : buffer_(capacity), capacity_(capacity), head_(0), tail_(0), count_(0) {}
    
    /**
     * @brief Добавить элемент в буфер (блокирующая операция)
     * 
     * Если буфер полон, поток будет ждать освобождения места.
     * 
     * @param item Элемент для добавления
     */
    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return count_ < capacity_; });
        
        buffer_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % capacity_;
        ++count_;
        
        lock.unlock();
        not_empty_.notify_one();
    }
    
    /**
     * @brief Извлечь элемент из буфера (блокирующая операция)
     * 
     * Если буфер пуст, поток будет ждать появления данных.
     * 
     * @return Извлеченный элемент
     */
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return count_ > 0 || stopped_; });
        
        if (stopped_ && count_ == 0) {
            return T(); // Возвращаем пустой элемент при остановке
        }
        
        T item = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        --count_;
        
        lock.unlock();
        not_full_.notify_one();
        
        return item;
    }
    
    /**
     * @brief Остановить буфер
     * 
     * Разблокирует все ожидающие потоки, чтобы они могли завершиться.
     */
    void stop() {
        std::unique_lock<std::mutex> lock(mutex_);
        stopped_ = true;
        lock.unlock();
        not_empty_.notify_all();
        not_full_.notify_all();
    }
    
    /**
     * @brief Проверить, остановлен ли буфер
     */
    bool is_stopped() const {
        return stopped_;
    }
    
    /**
     * @brief Получить текущее количество элементов
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    std::vector<T> buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t count_;
    bool stopped_ = false;
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
};

/**
 * @brief Блок данных для конвейерной обработки
 */
struct DataBlock {
    std::vector<char> data;           // Данные блока
    size_t offset = 0;                // Смещение в исходном файле
    size_t size = 0;                  // Размер данных
    bool is_last = false;             // Последний блок файла
    std::string error_message;        // Сообщение об ошибке (если есть)
    bool has_error = false;           // Флаг ошибки
    
    DataBlock() = default;
    explicit DataBlock(size_t block_size) : data(block_size) {}
};

/**
 * @brief Результат сжатия блока
 */
struct CompressedBlock {
    std::vector<char> gzip_data;      // Сжатые данные GZIP
    std::vector<char> brotli_data;    // Сжатые данные Brotli
    size_t original_size = 0;         // Исходный размер
    size_t gzip_size = 0;             // Размер после GZIP
    size_t brotli_size = 0;           // Размер после Brotli
    size_t offset = 0;                // Смещение блока
    bool is_last = false;             // Последний блок
    bool has_error = false;           // Флаг ошибки
    std::string error_message;        // Сообщение об ошибке
};

/**
 * @brief Конвейер сжатия файлов
 * 
 * Реализует трехэтапную обработку:
 * 1. Чтение: асинхронное чтение блоков из исходного файла
 * 2. Сжатие: параллельное сжатие в GZIP и Brotli
 * 3. Запись: асинхронная запись сжатых данных
 * 
 * Преимущества:
 * - Максимальная утилизация CPU и диска
 * - Устранение простоев между этапами
 * - Поддержка backpressure через кольцевые буферы
 */
class CompressionPipeline {
public:
    /**
     * @brief Конструктор конвейера
     * 
     * @param read_buffer_size Размер буфера между чтением и сжатием
     * @param compress_buffer_size Размер буфера между сжатием и записью
     * @param block_size Размер блока для чтения/сжатия (по умолчанию 1MB)
     */
    CompressionPipeline(
        size_t read_buffer_size = 4,
        size_t compress_buffer_size = 4,
        size_t block_size = 1024 * 1024
    );
    
    ~CompressionPipeline();
    
    /**
     * @brief Выполнить сжатие файла с использованием конвейера
     * 
     * @param input_path Путь к исходному файлу
     * @param gzip_output_path Путь для вывода GZIP
     * @param brotli_output_path Путь для вывода Brotli
     * @param compression_level Уровень сжатия (1-9)
     * @return true если успешно, false при ошибке
     */
    bool compress(
        const fs::path& input_path,
        const fs::path& gzip_output_path,
        const fs::path& brotli_output_path,
        int compression_level = 6
    );
    
    /**
     * @brief Остановить конвейер
     * 
     * Немедленно останавливает все этапы обработки.
     */
    void stop();
    
    /**
     * @brief Получить статистику работы конвейера
     */
    struct Statistics {
        size_t total_bytes_read = 0;
        size_t total_bytes_compressed_gzip = 0;
        size_t total_bytes_compressed_brotli = 0;
        size_t blocks_processed = 0;
        double read_time_ms = 0;
        double compress_time_ms = 0;
        double write_time_ms = 0;
    };
    
    Statistics get_statistics() const { return stats_; }

private:
    // Этапы конвейера
    void reader_stage(const fs::path& input_path);
    void compressor_stage(int compression_level);
    void writer_stage(
        const fs::path& gzip_output_path,
        const fs::path& brotli_output_path
    );
    
    // Буферы между этапами
    RingBuffer<DataBlock> read_to_compress_;
    RingBuffer<CompressedBlock> compress_to_write_;
    
    // Параметры
    size_t block_size_;
    
    // Потоки
    std::thread reader_thread_;
    std::thread compressor_thread_;
    std::thread writer_thread_;
    
    // Флаги управления
    std::atomic<bool> running_{false};
    std::atomic<bool> error_occurred_{false};
    
    // Статистика
    Statistics stats_;
    mutable std::mutex stats_mutex_;
    
    // Файловые дескрипторы
    int input_fd_ = -1;
    int gzip_output_fd_ = -1;
    int brotli_output_fd_ = -1;
};

#endif // PIPELINE_H
