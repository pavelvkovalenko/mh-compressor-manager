#include "pipeline.h"
#include "logger.h"
#include "i18n.h"
#include "security.h"
#include "compressor.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cstring>
#include <chrono>
#include <algorithm>
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif

#ifdef HAVE_LIBBROTLI
#include <brotli/encode.h>
#endif

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

// Переименованы макросы для избежания конфликта с <syslog.h>
#define PIPE_LOG_INFO(msg, ...) Logger::info(std::format(msg, ##__VA_ARGS__))
#define PIPE_LOG_ERROR(msg, ...) Logger::error(std::format(msg, ##__VA_ARGS__))
#define PIPE_LOG_WARN(msg, ...) Logger::warning(std::format(msg, ##__VA_ARGS__))
#define PIPE_LOG_DEBUG(msg, ...) Logger::debug(std::format(msg, ##__VA_ARGS__))

/**
 * @brief Конструктор конвейера
 */
CompressionPipeline::CompressionPipeline(
    size_t read_buffer_size,
    size_t compress_buffer_size,
    size_t block_size
)
    : read_to_compress_(read_buffer_size)
    , compress_to_write_(compress_buffer_size)
    , block_size_(block_size)
{
    PIPE_LOG_INFO(_("Compression pipeline initialized: read buffer={}, compress buffer={}, block size={}", "Конвейер сжатия инициализирован: буфер чтения={}, буфер сжатия={}, размер блока={}",
             read_buffer_size, compress_buffer_size, block_size);
}

/**
 * @brief Деструктор
 */
CompressionPipeline::~CompressionPipeline() {
    stop();
}

/**
 * @brief Остановка конвейера
 */
void CompressionPipeline::stop() {
    running_ = false;
    read_to_compress_.stop();
    compress_to_write_.stop();
    
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (compressor_thread_.joinable()) {
        compressor_thread_.join();
    }
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    
    if (input_fd_ >= 0) {
        close(input_fd_);
        input_fd_ = -1;
    }
    if (gzip_output_fd_ >= 0) {
        close(gzip_output_fd_);
        gzip_output_fd_ = -1;
    }
    if (brotli_output_fd_ >= 0) {
        close(brotli_output_fd_);
        brotli_output_fd_ = -1;
    }
}

/**
 * @brief Выполнить сжатие файла
 */
bool CompressionPipeline::compress(
    const fs::path& input_path,
    const fs::path& gzip_output_path,
    const fs::path& brotli_output_path,
    int compression_level
) {
    PIPE_LOG_INFO(_("Starting compression pipeline: {} -> {}, {}", "Запуск конвейера сжатия: {} -> {}, {}",
             input_path.string(), gzip_output_path.string(), brotli_output_path.string());
    
    // Проверка безопасности входного файла
    if (!security::validate_file_for_compression(input_path)) {
        PIPE_LOG_ERROR(_("Security check failed for file: {}", "Проверка безопасности не пройдена для файла: {}", input_path.string());
        return false;
    }
    
    // Открытие входного файла с защитой от TOCTOU
    input_fd_ = security::safe_open_file(input_path, O_RDONLY);
    if (input_fd_ < 0) {
        PIPE_LOG_ERROR(_("Failed to open file {}: {}", "Не удалось открыть файл {}: {}", input_path.string(), strerror(errno));
        return false;
    }
    
    // Получение размера файла и сохранение времени модификации
    struct stat st;
    if (fstat(input_fd_, &st) < 0) {
        PIPE_LOG_ERROR(_("Failed to get file size: {}", "Не удалось получить размер файла: {}", strerror(errno));
        close(input_fd_);
        input_fd_ = -1;
        return false;
    }
    // Сохраняем время модификации для копирования в сжатые файлы
    struct timespec orig_times[2];
    orig_times[0] = st.st_atim;  // atime
    orig_times[1] = st.st_mtim;  // mtime
    
    // Подсказки ядру для последовательного чтения
    posix_fadvise(input_fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
    posix_fadvise(input_fd_, 0, 0, POSIX_FADV_NOREUSE);
    
    // Создание выходных файлов через временные файлы для атомарности
    fs::path gzip_temp_path = gzip_output_path.string() + ".tmp";
    fs::path brotli_temp_path = brotli_output_path.string() + ".tmp";
    
    gzip_output_fd_ = open(gzip_temp_path.c_str(), 
                           O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                           0644);
    if (gzip_output_fd_ < 0) {
        PIPE_LOG_ERROR(_("Failed to create file {}: {}", "Не удалось создать файл {}: {}", gzip_temp_path.string(), strerror(errno));
        close(input_fd_);
        input_fd_ = -1;
        return false;
    }
    
    brotli_output_fd_ = open(brotli_temp_path.c_str(),
                             O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                             0644);
    if (brotli_output_fd_ < 0) {
        PIPE_LOG_ERROR(_("Failed to create file {}: {}", "Не удалось создать файл {}: {}", brotli_temp_path.string(), strerror(errno));
        close(gzip_output_fd_);
        close(input_fd_);
        gzip_output_fd_ = -1;
        input_fd_ = -1;
        fs::remove(gzip_temp_path);
        return false;
    }
    
    // Предварительное выделение места (опционально)
    // fallocate(gzip_output_fd_, 0, 0, file_size);
    // fallocate(brotli_output_fd_, 0, 0, file_size);
    
    // Запуск потоков конвейера
    running_ = true;
    error_occurred_ = false;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    reader_thread_ = std::thread(&CompressionPipeline::reader_stage, this, input_path);
    compressor_thread_ = std::thread(&CompressionPipeline::compressor_stage, this, compression_level);
    writer_thread_ = std::thread(&CompressionPipeline::writer_stage, this, 
                                  gzip_output_path, brotli_output_path);
    
    // Ожидание завершения потоков
    reader_thread_.join();
    compressor_thread_.join();
    writer_thread_.join();
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    // Закрытие файлов
    close(input_fd_);
    close(gzip_output_fd_);
    close(brotli_output_fd_);
    input_fd_ = -1;
    gzip_output_fd_ = -1;
    brotli_output_fd_ = -1;
    
    if (error_occurred_) {
        PIPE_LOG_ERROR(_("Compression pipeline completed with error. Time: {} ms", "Конвейер сжатия завершен с ошибкой. Время работы: {} мс", total_time);
        // Удаление временных файлов при ошибке
        fs::remove(gzip_temp_path);
        fs::remove(brotli_temp_path);
        return false;
    }
    
    // Атомарное переименование временных файлов в целевые
    try {
        fs::rename(gzip_temp_path, gzip_output_path);
        fs::rename(brotli_temp_path, brotli_output_path);
    } catch (const fs::filesystem_error& e) {
        PIPE_LOG_ERROR(_("File rename error: {}", "Ошибка переименования файлов: {}", e.what());
        fs::remove(gzip_temp_path);
        fs::remove(brotli_temp_path);
        return false;
    }

    // Копирование временных меток оригинала в сжатые файлы.
    try {
        utimensat(AT_FDCWD, gzip_output_path.c_str(), orig_times, 0);
        utimensat(AT_FDCWD, brotli_output_path.c_str(), orig_times, 0);
    } catch (const std::exception& e) {
        PIPE_LOG_ERROR(_("Failed to set modification time: {}", "Не удалось установить время модификации: {}", e.what());
    }
    
    PIPE_LOG_INFO(_("Compression pipeline completed successfully. Time: {} ms, read: {} bytes, "
             "GZIP: {} bytes, Brotli: {} bytes", "Конвейер сжатия успешно завершен. Время: {} мс, прочитано: {} байт, "
             "GZIP: {} байт, Brotli: {} байт",
             total_time,
             stats_.total_bytes_read,
             stats_.total_bytes_compressed_gzip,
             stats_.total_bytes_compressed_brotli);
    
    return true;
}

/**
 * @brief Этап чтения
 * 
 * Читает данные из входного файла блоками и передает их в буфер сжатия.
 * Реализует передачу "хвоста" предыдущего блока для контекстного сжатия.
 */
void CompressionPipeline::reader_stage(const fs::path& input_path) {
    auto start_time = std::chrono::high_resolution_clock::now();
    size_t total_read = 0;
    size_t offset = 0;
    
    try {
        while (running_ && !error_occurred_) {
            DataBlock block(block_size_);
            block.offset = offset;
            
            // Добавляем хвост предыдущего блока для контекстного сжатия
            {
                std::lock_guard<std::mutex> lock(tail_mutex_);
                if (!previous_tail_.empty()) {
                    block.prev_tail = previous_tail_;
                }
            }
            
            // Чтение данных
            ssize_t bytes_read = read(input_fd_, block.data.data(), block_size_);
            
            if (bytes_read < 0) {
                if (errno == EINTR) continue;
                // Обработка удаления файла пользователем во время чтения
                if (errno == ENOENT || errno == EBADF) {
                    PIPE_LOG_WARN(_("File {} was deleted or became unavailable during read", "Файл {} был удален или стал недоступен во время чтения", input_path.string());
                    error_occurred_ = true;
                    block.has_error = true;
                    block.error_message = "Файл удален пользователем";
                    read_to_compress_.push(std::move(block));
                    break;
                }
                PIPE_LOG_ERROR(_("File read error {}: {}", "Ошибка чтения файла {}: {}", input_path.string(), strerror(errno));
                error_occurred_ = true;
                block.has_error = true;
                block.error_message = strerror(errno);
                read_to_compress_.push(std::move(block));
                break;
            }
            
            if (bytes_read == 0) {
                // Конец файла - отправляем последний пустой блок
                block.is_last = true;
                block.size = 0;
                read_to_compress_.push(std::move(block));
                break;
            }
            
            block.size = static_cast<size_t>(bytes_read);
            total_read += bytes_read;
            offset += bytes_read;
            
            // Сохраняем хвост текущего блока для следующего блока
            {
                std::lock_guard<std::mutex> lock(tail_mutex_);
                if (bytes_read > static_cast<ssize_t>(CONTEXT_TAIL_SIZE)) {
                    // Сохраняем последние CONTEXT_TAIL_SIZE байт
                    previous_tail_.assign(
                        block.data.begin() + (bytes_read - CONTEXT_TAIL_SIZE),
                        block.data.end()
                    );
                } else {
                    // Сохраняем весь блок если он меньше CONTEXT_TAIL_SIZE
                    previous_tail_.assign(
                        block.data.begin(),
                        block.data.begin() + bytes_read
                    );
                }
            }
            
            read_to_compress_.push(std::move(block));
        }
    } catch (const std::exception& e) {
        PIPE_LOG_ERROR(_("Exception in read thread: {}", "Исключение в потоке чтения: {}", e.what());
        error_occurred_ = true;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count() / 1000.0;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_bytes_read = total_read;
    stats_.read_time_ms = elapsed;
}

/**
 * @brief Этап сжатия
 * 
 * Получает блоки из буфера чтения, сжимает их в GZIP и Brotli,
 * затем передает в буфер записи.
 */
void CompressionPipeline::compressor_stage([[maybe_unused]] int compression_level) {
    auto start_time = std::chrono::high_resolution_clock::now();
    size_t blocks_processed = 0;
    
    try {
        while (true) {
            DataBlock block = read_to_compress_.pop();
            
            // Проверка на окончание потока
            if (block.is_last && block.size == 0) {
                // Отправляем сигнал окончания записи
                CompressedBlock compressed;
                compressed.is_last = true;
                compressed.original_size = 0;
                compress_to_write_.push(std::move(compressed));
                break;
            }
            
            if (read_to_compress_.is_stopped() && block.size == 0) {
                break;
            }
            
            CompressedBlock compressed;
            compressed.offset = block.offset;
            compressed.original_size = block.size;
            compressed.is_last = block.is_last;
            
            if (block.has_error) {
                compressed.has_error = true;
                compressed.error_message = block.error_message;
                compress_to_write_.push(std::move(compressed));
                error_occurred_ = true;
                continue;
            }
            
            // Сжатие GZIP с использованием контекста предыдущего блока
#ifdef HAVE_ZLIB
            {
                // Объединяем хвост предыдущего блока с текущими данными для лучшего сжатия
                std::vector<uint8_t> input_data;
                if (!block.prev_tail.empty()) {
                    input_data.reserve(block.prev_tail.size() + block.size);
                    input_data.insert(input_data.end(), 
                                     reinterpret_cast<uint8_t*>(block.prev_tail.data()),
                                     reinterpret_cast<uint8_t*>(block.prev_tail.data()) + block.prev_tail.size());
                    input_data.insert(input_data.end(),
                                     reinterpret_cast<uint8_t*>(block.data.data()),
                                     reinterpret_cast<uint8_t*>(block.data.data()) + block.size);
                } else {
                    input_data.reserve(block.size);
                    input_data.insert(input_data.end(),
                                     reinterpret_cast<uint8_t*>(block.data.data()),
                                     reinterpret_cast<uint8_t*>(block.data.data()) + block.size);
                }
                
                compressed.gzip_data.resize(compressBound(input_data.size()));
                z_stream strm = {};
                if (deflateInit2(&strm, compression_level, Z_DEFLATED, MAX_WBITS + 16, 8,
                                Z_DEFAULT_STRATEGY) == Z_OK) {
                    strm.next_in = reinterpret_cast<Bytef*>(input_data.data());
                    strm.avail_in = input_data.size();
                    strm.next_out = reinterpret_cast<Bytef*>(compressed.gzip_data.data());
                    strm.avail_out = compressed.gzip_data.size();
                    
                    if (deflate(&strm, Z_FINISH) == Z_STREAM_END) {
                        compressed.gzip_data.resize(strm.total_out);
                        compressed.gzip_size = strm.total_out;
                    } else {
                        PIPE_LOG_ERROR(_("GZIP compression error", "Ошибка сжатия GZIP"));
                        compressed.has_error = true;
                    }
                    deflateEnd(&strm);
                } else {
                    PIPE_LOG_ERROR(_("Failed to initialize GZIP", "Не удалось инициализировать GZIP"));
                    compressed.has_error = true;
                }
            }
#else
            compressed.has_error = true;
            compressed.error_message = "ZLIB не доступен";
#endif
            
            // Сжатие Brotli с использованием контекста предыдущего блока
#ifdef HAVE_LIBBROTLI
            {
                // Объединяем хвост предыдущего блока с текущими данными для лучшего сжатия
                std::vector<uint8_t> input_data;
                if (!block.prev_tail.empty()) {
                    input_data.reserve(block.prev_tail.size() + block.size);
                    input_data.insert(input_data.end(), 
                                     reinterpret_cast<uint8_t*>(block.prev_tail.data()),
                                     reinterpret_cast<uint8_t*>(block.prev_tail.data()) + block.prev_tail.size());
                    input_data.insert(input_data.end(),
                                     reinterpret_cast<uint8_t*>(block.data.data()),
                                     reinterpret_cast<uint8_t*>(block.data.data()) + block.size);
                } else {
                    input_data.reserve(block.size);
                    input_data.insert(input_data.end(),
                                     reinterpret_cast<uint8_t*>(block.data.data()),
                                     reinterpret_cast<uint8_t*>(block.data.data()) + block.size);
                }
                
                size_t encoded_size = BrotliEncoderMaxCompressedSize(input_data.size());
                compressed.brotli_data.resize(encoded_size);
                
                const uint8_t* input_buffer = reinterpret_cast<const uint8_t*>(input_data.data());
                uint8_t* output_buffer = reinterpret_cast<uint8_t*>(compressed.brotli_data.data());
                
                if (BrotliEncoderCompress(compression_level, BROTLI_DEFAULT_WINDOW, 
                                          BROTLI_DEFAULT_MODE, input_data.size(), input_buffer,
                                          &encoded_size, output_buffer)) {
                    compressed.brotli_data.resize(encoded_size);
                    compressed.brotli_size = encoded_size;
                } else {
                    PIPE_LOG_ERROR(_("Brotli compression error", "Ошибка сжатия Brotli"));
                    compressed.has_error = true;
                }
            }
#else
            compressed.has_error = true;
            compressed.error_message = "Brotli не доступен";
#endif
            
            if (compressed.has_error) {
                error_occurred_ = true;
            }
            
            blocks_processed++;
            compress_to_write_.push(std::move(compressed));
        }
    } catch (const std::exception& e) {
        PIPE_LOG_ERROR(_("Exception in compression thread: {}", "Исключение в потоке сжатия: {}", e.what());
        error_occurred_ = true;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count() / 1000.0;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.blocks_processed = blocks_processed;
    stats_.compress_time_ms = elapsed;
}

/**
 * @brief Этап записи
 * 
 * Получает сжатые блоки из буфера и записывает их в выходные файлы.
 */
void CompressionPipeline::writer_stage(
    [[maybe_unused]] const fs::path& gzip_output_path,
    [[maybe_unused]] const fs::path& brotli_output_path
) {
    auto start_time = std::chrono::high_resolution_clock::now();
    size_t total_gzip = 0;
    size_t total_brotli = 0;
    
    try {
        while (true) {
            CompressedBlock block = compress_to_write_.pop();
            
            if (block.is_last && block.original_size == 0) {
                break;
            }
            
            if (compress_to_write_.is_stopped() && block.original_size == 0) {
                break;
            }
            
            if (block.has_error) {
                PIPE_LOG_ERROR(_("Error block, skipping write", "Блок с ошибкой, пропуск записи"));
                error_occurred_ = true;
                continue;
            }
            
            // Запись GZIP
            if (!block.gzip_data.empty()) {
                ssize_t written = write(gzip_output_fd_, block.gzip_data.data(), block.gzip_data.size());
                if (written < 0) {
                    // Обработка удаления выходного файла пользователем
                    if (errno == EBADF || errno == EIO) {
                        PIPE_LOG_WARN(_("GZIP output file was deleted or became unavailable during write", "Выходной файл GZIP был удален или стал недоступен во время записи"));
                        error_occurred_ = true;
                        break;
                    }
                    PIPE_LOG_ERROR(_("GZIP write error: {}", "Ошибка записи GZIP: {}", strerror(errno));
                    error_occurred_ = true;
                    break;
                }
                total_gzip += written;
            }
            
            // Запись Brotli
            if (!block.brotli_data.empty()) {
                ssize_t written = write(brotli_output_fd_, block.brotli_data.data(), block.brotli_data.size());
                if (written < 0) {
                    // Обработка удаления выходного файла пользователем
                    if (errno == EBADF || errno == EIO) {
                        PIPE_LOG_WARN(_("Brotli output file was deleted or became unavailable during write", "Выходной файл Brotli был удален или стал недоступен во время записи"));
                        error_occurred_ = true;
                        break;
                    }
                    PIPE_LOG_ERROR(_("Brotli write error: {}", "Ошибка записи Brotli: {}", strerror(errno));
                    error_occurred_ = true;
                    break;
                }
                total_brotli += written;
            }
        }
        
        // Синхронизация данных на диске
        fsync(gzip_output_fd_);
        fsync(brotli_output_fd_);
        
    } catch (const std::exception& e) {
        PIPE_LOG_ERROR(_("Exception in write thread: {}", "Исключение в потоке записи: {}", e.what());
        error_occurred_ = true;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count() / 1000.0;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_bytes_compressed_gzip = total_gzip;
    stats_.total_bytes_compressed_brotli = total_brotli;
    stats_.write_time_ms = elapsed;
}
