#include "compressor.h"
#include "logger.h"
#include "memory_pool.h"
#include <fstream>
#include <vector>
#include <zlib.h>
#include <brotli/encode.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif
#include <algorithm>
#include <cerrno>
#include <cstring>      // Для strerror()
#include <pwd.h>        // Для getpwuid()
#include <grp.h>        // Для getgrgid()
#include <climits>      // Для PATH_MAX
#include <set>          // Для кэша валидации путей
#include <atomic>       // Для потокобезопасности
#include <linux/aio_abi.h>
#include <libaio.h>
#include <random>       // Для генерации случайных имен временных файлов

// Размер буфера для потоковой обработки (256KB - оптимизировано для производительности)
constexpr size_t STREAM_BUFFER_SIZE = 262144;

// Максимальное количество попыток при проверке пути для предотвращения DoS
constexpr int MAX_PATH_VALIDATION_RETRIES = 3;

// Буфер для /proc/self/fd/<fd> путей (достаточно для большинства систем)
constexpr size_t PROC_FD_PATH_SIZE = 64;

// Максимальный размер файла для сжатия (100MB) - защита от DoS атак
constexpr uint64_t MAX_FILE_SIZE = 100 * 1024 * 1024;

// Глобальный пул памяти для буферов сжатия (переиспользуется между задачами)
static ByteBufferPool g_buffer_pool(16);

// Флаги для Direct I/O
constexpr int DIRECT_IO_FLAGS = O_DIRECT;

// Максимальное количество попыток создания временного файла
constexpr int MAX_TEMP_FILE_RETRIES = 10;

// Размер случайной части имени временного файла (байты)
constexpr size_t TEMP_FILE_RANDOM_BYTES = 8;

bool Compressor::compress_gzip(const fs::path& input, const fs::path& output, int level) {
    // Проверка диапазона уровня сжатия (защита от некорректных значений)
    if (level < 1 || level > 9) {
        Logger::warning(std::format("Invalid gzip level {}, using default 6", level));
        level = 6;
    }
    
    // === ЗАЩИТА ОТ TOCTOU: Используем openat() с дескриптором директории ===
    // Получаем директорию и базовое имя файла
    fs::path parent_dir = input.parent_path();
    std::string basename = input.filename().string();
    
    // Если путь относительный или пустой, используем текущую директорию
    if (parent_dir.empty()) {
        parent_dir = ".";
    }
    
    // Открываем директорию с O_RDONLY | O_DIRECTORY | O_NOFOLLOW для защиты от symlink
    int dir_fd = open(parent_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0) {
        Logger::error(std::format("Failed to open directory {}: {}", parent_dir.string(), strerror(errno)));
        return false;
    }
    
    // Шаг 1: Открываем файл с O_PATH через openat() для получения fd без чтения данных
    // Это полностью устраняет TOCTOU - атаки между проверкой пути и открытием
    int fd_path = openat(dir_fd, basename.c_str(), O_PATH | O_NOFOLLOW | O_NOATIME);
    if (fd_path < 0) {
        int saved_errno = errno;
        close(dir_fd);
        if (saved_errno == ELOOP || saved_errno == EMLINK) {
            Logger::error(std::format("Symlink attack detected: {} - refusing to open", input.string()));
            return false;
        }
        Logger::error(std::format("Failed to open file reference: {} - {}", input.string(), strerror(saved_errno)));
        return false;
    }
    
    // Закрываем dir_fd, он больше не нужен
    close(dir_fd);
    
    // Шаг 2: Проверяем что это обычный файл через fstat (не следует за symlink)
    struct stat st;
    if (fstat(fd_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd_path);
        Logger::error(std::format("Path is not a regular file or is a symlink: {}", input.string()));
        return false;
    }
    
    // Проверка на максимальный размер файла для предотвращения DoS атак
    if (st.st_size > MAX_FILE_SIZE) {
        close(fd_path);
        Logger::warning(std::format("File too large: {} (size: {} bytes, max: {} bytes)", 
                                    input.string(), st.st_size, MAX_FILE_SIZE));
        return false;
    }
    
    // Шаг 3: Дополнительная проверка на hardlink-атаки (st_nlink > 1 может указывать на атаку)
    if (st.st_nlink > 1) {
        close(fd_path);
        Logger::warning(std::format("Hardlink detected (nlink={}): {} - potential security risk", st.st_nlink, input.string()));
        // Не блокируем, но логируем для аудита
    }
    
    // Проверка прав доступа перед чтением
    if ((st.st_mode & S_IRUSR) == 0 && getuid() != st.st_uid && getuid() != 0) {
        close(fd_path);
        Logger::warning(std::format("No read permission for file: {}", input.string()));
        return false;
    }
    
    // Шаг 4: Открываем файл для чтения через /proc/self/fd/ используя тот же fd
    // Это гарантирует что мы работаем с тем же самым файлом, который проверили
    char proc_path[PROC_FD_PATH_SIZE];
    int ret = snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd_path);
    if (ret < 0 || static_cast<size_t>(ret) >= sizeof(proc_path)) {
        int saved_errno = errno;
        close(fd_path);
        Logger::error(std::format("snprintf failed for proc path: {} - {}", input.string(), strerror(saved_errno)));
        return false;
    }
    
    int fd_in = open(proc_path, O_RDONLY | O_NOATIME);
    if (fd_in < 0) {
        int saved_errno = errno;
        close(fd_path);
        Logger::error(std::format("Failed to reopen file for reading: {} - {}", input.string(), strerror(saved_errno)));
        return false;
    }
    
    // Закрываем временный fd_path, он больше не нужен
    close(fd_path);
    
    // Подсказка ОС о последовательном чтении и предзагрузке
    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);
    
    // Шаг 5: Используем fdopen() для работы с FILE* из уже открытого fd
    // Это полностью устраняет TOCTOU - между проверкой и открытием нет race condition
    FILE* file_in = fdopen(fd_in, "rb");
    if (!file_in) {
        int saved_errno = errno;
        close(fd_in);
        Logger::error(std::format("Failed to fdopen file {}: {}", input.string(), strerror(saved_errno)));
        return false;
    }
    // fdopen забирает владение дескриптором, теперь закрывать нужно только fclose(file_in)

    // Шаг 6: Безопасное создание выходного файла через временный файл + rename()
    // Это полностью устраняет race condition между open() с O_EXCL и последующими операциями
    // Используем временный файл в той же директории что и целевой файл
    
    fs::path output_parent = output.parent_path();
    if (output_parent.empty()) {
        output_parent = ".";
    }
    
    // Генерируем случайное имя для временного файла
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    
    fs::path temp_path;
    int fd_out = -1;
    bool temp_created = false;
    
    for (int retry = 0; retry < MAX_TEMP_FILE_RETRIES && !temp_created; ++retry) {
        // Создаем уникальное имя временного файла: <output>.tmp.<random_hex>
        uint64_t random_val = dist(gen);
        char random_hex[TEMP_FILE_RANDOM_BYTES * 2 + 1];
        snprintf(random_hex, sizeof(random_hex), "%016lx", random_val);
        
        temp_path = output.string() + ".tmp." + std::string(random_hex);
        
        // Открываем временный файл с O_CREAT | O_EXCL для атомарности
        fd_out = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | DIRECT_IO_FLAGS, st.st_mode & 0666);
        
        if (fd_out >= 0) {
            temp_created = true;
        } else {
            int saved_errno = errno;
            if (saved_errno == EINVAL) {
                // O_DIRECT не поддерживается - пробуем без него
                Logger::debug(std::format("O_DIRECT not supported for temp file {}, trying without direct I/O", temp_path.string()));
                fd_out = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, st.st_mode & 0666);
                if (fd_out >= 0) {
                    temp_created = true;
                }
            } else if (saved_errno != EEXIST) {
                // Другая ошибка - логируем и пробуем снова
                Logger::warning(std::format("Failed to create temp file (attempt {}/{}): {} - {}", 
                                           retry + 1, MAX_TEMP_FILE_RETRIES, temp_path.string(), strerror(saved_errno)));
            }
            // Если EEXIST - цикл продолжится с новым случайным именем
        }
    }
    
    if (!temp_created || fd_out < 0) {
        fclose(file_in);
        Logger::error(std::format("Failed to create temporary file after {} attempts: {}", 
                                 MAX_TEMP_FILE_RETRIES, output.string()));
        return false;
    }
    
    FILE* file_out = fdopen(fd_out, "wb");
    if (!file_out) {
        close(fd_out);
        fclose(file_in);
        Logger::error(std::format("Failed to fdopen temp file {}: {}", temp_path.string(), strerror(errno)));
        // Удаляем временный файл при ошибке
        unlink(temp_path.c_str());
        return false;
    }

    z_stream strm = {};
    // Используем Z_HUFFMAN_ONLY для быстрых файлов или Z_DEFAULT_STRATEGY для лучшего сжатия
    int strategy = (level <= 3) ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, strategy) != Z_OK) {
        Logger::error("Failed to init gzip stream");
        fclose(file_in);
        fclose(file_out);
        return false;
    }

    // Выделяем буферы один раз вне цикла
    std::vector<char> in_buffer(STREAM_BUFFER_SIZE);
    std::vector<char> out_buffer(STREAM_BUFFER_SIZE);
    
    int ret;
    bool has_error = false;

    do {
        // Читаем порциями из входного файла через fdopen FILE*
        size_t bytes_read = fread(in_buffer.data(), 1, in_buffer.size(), file_in);
        
        if (bytes_read > 0) {
            strm.avail_in = bytes_read;
            strm.next_in = (Bytef*)in_buffer.data();
            
            // Сжимаем и записываем порциями
            do {
                strm.avail_out = out_buffer.size();
                strm.next_out = (Bytef*)out_buffer.data();
                ret = deflate(&strm, feof(file_in) ? Z_FINISH : Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR) {
                    Logger::error("Gzip stream error");
                    has_error = true;
                    break;
                }
                size_t have = out_buffer.size() - strm.avail_out;
                if (have > 0) {
                    if (fwrite(out_buffer.data(), 1, have, file_out) != have) {
                        // Проверяем errno для определения причины ошибки
                        if (errno == ENOSPC) {
                            Logger::error(std::format("No space left on device while writing gzip output: {}", output.string()));
                        } else {
                            Logger::error(std::format("Failed to write gzip output: {} - {}", output.string(), strerror(errno)));
                        }
                        has_error = true;
                        break;
                    }
                }
            } while (strm.avail_out == 0 && !has_error);
        }
        
        if (has_error) break;
    } while (!feof(file_in));

    // Завершаем поток (если не было ошибок)
    if (!has_error) {
        do {
            strm.avail_out = out_buffer.size();
            strm.next_out = (Bytef*)out_buffer.data();
            ret = deflate(&strm, Z_FINISH);
            if (ret == Z_STREAM_ERROR) {
                Logger::error("Gzip stream error");
                has_error = true;
                break;
            }
            size_t have = out_buffer.size() - strm.avail_out;
            if (have > 0) {
                if (fwrite(out_buffer.data(), 1, have, file_out) != have) {
                    // Проверяем errno для определения причины ошибки
                    if (errno == ENOSPC) {
                        Logger::error(std::format("No space left on device while writing gzip output: {}", output.string()));
                    } else {
                        Logger::error(std::format("Failed to write gzip output: {} - {}", output.string(), strerror(errno)));
                    }
                    has_error = true;
                    break;
                }
            }
        } while (ret == Z_OK);
    }

    deflateEnd(&strm);
    fclose(file_in);  // Закрываем FILE*, fdopen забрал дескриптор
    
    // Проверяем ошибки записи и делаем fsync перед rename
    if (has_error || fflush(file_out) != 0) {
        if (errno == ENOSPC) {
            Logger::error(std::format("No space left on device while finalizing gzip output: {}", output.string()));
        } else {
            Logger::error("Failed to write gzip output");
        }
        fclose(file_out);
        unlink(temp_path.c_str());  // Удаляем временный файл при ошибке
        return false;
    }
    
    // Получаем дескриптор из FILE* для fsync перед rename
    int out_fd_raw = fileno(file_out);
    if (out_fd_raw >= 0 && fsync(out_fd_raw) != 0) {
        if (errno == ENOSPC) {
            Logger::error(std::format("No space left on device while fsync gzip output: {}", output.string()));
        } else {
            Logger::error(std::format("Failed to fsync gzip output: {}", strerror(errno)));
        }
        fclose(file_out);
        unlink(temp_path.c_str());  // Удаляем временный файл при ошибке
        return false;
    }
    
    fclose(file_out);
    
    // Атомарно переименовываем временный файл в целевой
    // Это завершает защиту от race condition - файл появляется только после успешной записи
    if (rename(temp_path.c_str(), output.c_str()) != 0) {
        Logger::error(std::format("Failed to rename temp file to output: {} -> {} - {}", 
                                 temp_path.string(), output.string(), strerror(errno)));
        unlink(temp_path.c_str());  // Удаляем временный файл при ошибке
        return false;
    }

    Logger::debug(std::format("Gzip compressed: {} -> {} (via temp file)", input.string(), output.string()));
    return true;
}

bool Compressor::compress_brotli(const fs::path& input, const fs::path& output, int level) {
    // Проверка диапазона уровня сжатия (защита от некорректных значений)
    if (level < 1 || level > 11) {
        Logger::warning(std::format("Invalid brotli level {}, using default 4", level));
        level = 4;
    }
    
    // === ЗАЩИТА ОТ TOCTOU: Используем openat() с дескриптором директории ===
    // Получаем директорию и базовое имя файла
    fs::path parent_dir = input.parent_path();
    std::string basename = input.filename().string();
    
    // Если путь относительный или пустой, используем текущую директорию
    if (parent_dir.empty()) {
        parent_dir = ".";
    }
    
    // Открываем директорию с O_RDONLY | O_DIRECTORY | O_NOFOLLOW для защиты от symlink
    int dir_fd = open(parent_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0) {
        Logger::error(std::format("Failed to open directory {}: {}", parent_dir.string(), strerror(errno)));
        return false;
    }
    
    // Шаг 1: Открываем файл с O_PATH через openat() для получения fd без чтения данных
    // Это полностью устраняет TOCTOU - атаки между проверкой пути и открытием
    int fd_path = openat(dir_fd, basename.c_str(), O_PATH | O_NOFOLLOW | O_NOATIME);
    if (fd_path < 0) {
        int saved_errno = errno;
        close(dir_fd);
        if (saved_errno == ELOOP || saved_errno == EMLINK) {
            Logger::error(std::format("Symlink attack detected: {} - refusing to open", input.string()));
            return false;
        }
        Logger::error(std::format("Failed to open file reference: {} - {}", input.string(), strerror(saved_errno)));
        return false;
    }
    
    // Закрываем dir_fd, он больше не нужен
    close(dir_fd);
    
    // Шаг 2: Проверяем что это обычный файл через fstat
    struct stat st;
    if (fstat(fd_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd_path);
        Logger::error(std::format("Path is not a regular file or is a symlink: {}", input.string()));
        return false;
    }
    
    // Проверка на максимальный размер файла для предотвращения DoS атак
    if (st.st_size > MAX_FILE_SIZE) {
        close(fd_path);
        Logger::warning(std::format("File too large: {} (size: {} bytes, max: {} bytes)", 
                                    input.string(), st.st_size, MAX_FILE_SIZE));
        return false;
    }
    
    // Шаг 3: Дополнительная проверка на hardlink-атаки
    if (st.st_nlink > 1) {
        close(fd_path);
        Logger::warning(std::format("Hardlink detected (nlink={}): {} - potential security risk", st.st_nlink, input.string()));
    }
    
    // Проверка прав доступа перед чтением
    if ((st.st_mode & S_IRUSR) == 0 && getuid() != st.st_uid && getuid() != 0) {
        close(fd_path);
        Logger::warning(std::format("No read permission for file: {}", input.string()));
        return false;
    }
    
    // Шаг 4: Открываем файл для чтения через /proc/self/fd/
    char proc_path[PROC_FD_PATH_SIZE];
    int ret = snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd_path);
    if (ret < 0 || static_cast<size_t>(ret) >= sizeof(proc_path)) {
        int saved_errno = errno;
        close(fd_path);
        Logger::error(std::format("snprintf failed for proc path: {} - {}", input.string(), strerror(saved_errno)));
        return false;
    }
    
    int fd_in = open(proc_path, O_RDONLY | O_NOATIME);
    if (fd_in < 0) {
        int saved_errno = errno;
        close(fd_path);
        Logger::error(std::format("Failed to reopen file for reading: {} - {}", input.string(), strerror(saved_errno)));
        return false;
    }
    
    close(fd_path);
    
    // Подсказка ОС о последовательном чтении и предзагрузке
    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);
    
    // Шаг 5: Используем fdopen() для работы с FILE* из уже открытого fd
    FILE* file_in = fdopen(fd_in, "rb");
    if (!file_in) {
        int saved_errno = errno;
        close(fd_in);
        Logger::error(std::format("Failed to fdopen file {}: {}", input.string(), strerror(saved_errno)));
        return false;
    }
    // fdopen забирает владение дескриптором

    // Шаг 6: Безопасное создание выходного файла через временный файл + rename() (аналогично gzip)
    fs::path output_parent = output.parent_path();
    if (output_parent.empty()) {
        output_parent = ".";
    }
    
    // Генерируем случайное имя для временного файла
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    
    fs::path temp_path;
    int fd_out = -1;
    bool temp_created = false;
    
    for (int retry = 0; retry < MAX_TEMP_FILE_RETRIES && !temp_created; ++retry) {
        uint64_t random_val = dist(gen);
        char random_hex[TEMP_FILE_RANDOM_BYTES * 2 + 1];
        snprintf(random_hex, sizeof(random_hex), "%016lx", random_val);
        
        temp_path = output.string() + ".tmp." + std::string(random_hex);
        
        // Пытаемся создать с O_DIRECT, если не поддерживается - пробуем без него
        fd_out = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | DIRECT_IO_FLAGS, st.st_mode & 0666);
        
        if (fd_out >= 0) {
            temp_created = true;
        } else {
            int saved_errno = errno;
            if (saved_errno == EINVAL) {
                // O_DIRECT не поддерживается - пробуем без него
                Logger::debug(std::format("O_DIRECT not supported for temp file {}, trying without direct I/O", temp_path.string()));
                fd_out = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, st.st_mode & 0666);
                if (fd_out >= 0) {
                    temp_created = true;
                }
            } else if (saved_errno != EEXIST) {
                // Другая ошибка - логируем и пробуем снова
                Logger::warning(std::format("Failed to create temp file for brotli (attempt {}/{}): {} - {}", 
                                           retry + 1, MAX_TEMP_FILE_RETRIES, temp_path.string(), strerror(saved_errno)));
            }
            // Если EEXIST - цикл продолжится с новым случайным именем
        }
    }
    
    if (!temp_created || fd_out < 0) {
        fclose(file_in);
        Logger::error(std::format("Failed to create temporary file after {} attempts: {}", 
                                 MAX_TEMP_FILE_RETRIES, output.string()));
        return false;
    }
    
    FILE* file_out = fdopen(fd_out, "wb");
    if (!file_out) {
        close(fd_out);
        fclose(file_in);
        Logger::error(std::format("Failed to fdopen temp file {}: {}", temp_path.string(), strerror(errno)));
        unlink(temp_path.c_str());
        return false;
    }

    // Создаем поток сжатия Brotli
    BrotliEncoderState* state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        fclose(file_in);
        fclose(file_out);
        Logger::error("Failed to create Brotli encoder");
        return false;
    }

    BrotliEncoderSetParameter(state, BROTLI_PARAM_QUALITY, (uint32_t)level);
    // Оптимизация: используем режим потока для лучшей производительности
    BrotliEncoderSetParameter(state, BROTLI_PARAM_MODE, BROTLI_MODE_TEXT);
    BrotliEncoderSetParameter(state, BROTLI_PARAM_SIZE_HINT, 0);

    // Выделяем буферы один раз вне цикла
    std::vector<uint8_t> in_buffer(STREAM_BUFFER_SIZE);
    std::vector<uint8_t> out_buffer(STREAM_BUFFER_SIZE);
    bool success = true;

    while (true) {
        // Читаем порцию из входного файла через fdopen FILE*
        size_t bytes_read = fread(in_buffer.data(), 1, in_buffer.size(), file_in);
        
        const uint8_t* next_in = in_buffer.data();
        size_t available_in = bytes_read;
        
        // Обрабатываем прочитанные данные
        if (bytes_read > 0 || !feof(file_in)) {
            while (available_in > 0 || !BrotliEncoderIsFinished(state)) {
                size_t available_out = out_buffer.size();
                uint8_t* next_out = out_buffer.data();
                
                if (!BrotliEncoderCompressStream(state,
                        feof(file_in) ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS,
                        &available_in, &next_in,
                        &available_out, &next_out,
                        nullptr)) {
                    Logger::error(std::format("Brotli compression stream error for: {}", input.string()));
                    success = false;
                    break;
                }
                
                size_t written = out_buffer.size() - available_out;
                if (written > 0) {
                    if (fwrite(out_buffer.data(), 1, written, file_out) != written) {
                        if (errno == ENOSPC) {
                            Logger::error(std::format("No space left on device while writing brotli output: {}", output.string()));
                        } else {
                            Logger::error(std::format("Failed to write brotli output: {} - {}", output.string(), strerror(errno)));
                        }
                        success = false;
                        break;
                    }
                }
                
                if (available_in == 0 && !feof(file_in)) {
                    break;
                }
                
                if (BrotliEncoderIsFinished(state)) {
                    break;
                }
            }
        }
        
        if (!success) break;
        
        // Если достигнут конец файла и поток завершен
        if (feof(file_in) && BrotliEncoderIsFinished(state)) {
            break;
        }
    }

    BrotliEncoderDestroyInstance(state);
    fclose(file_in);  // Закрываем FILE*, fdopen забрал дескриптор
    
    // Проверяем ошибки записи и делаем fsync перед rename
    if (!success || fflush(file_out) != 0) {
        if (errno == ENOSPC) {
            Logger::error(std::format("No space left on device while finalizing brotli output: {}", output.string()));
        } else {
            Logger::error("Failed to write brotli output");
        }
        fclose(file_out);
        unlink(temp_path.c_str());  // Удаляем временный файл при ошибке
        return false;
    }
    
    // Получаем дескриптор из FILE* для fsync перед rename
    int out_fd_raw = fileno(file_out);
    if (out_fd_raw >= 0 && fsync(out_fd_raw) != 0) {
        if (errno == ENOSPC) {
            Logger::error(std::format("No space left on device while fsync brotli output: {}", output.string()));
        } else {
            Logger::error(std::format("Failed to fsync brotli output: {}", strerror(errno)));
        }
        fclose(file_out);
        unlink(temp_path.c_str());  // Удаляем временный файл при ошибке
        return false;
    }
    
    fclose(file_out);
    
    // Атомарно переименовываем временный файл в целевой
    if (rename(temp_path.c_str(), output.c_str()) != 0) {
        Logger::error(std::format("Failed to rename temp file to output: {} -> {} - {}", 
                                 temp_path.string(), output.string(), strerror(errno)));
        unlink(temp_path.c_str());  // Удаляем временный файл при ошибке
        return false;
    }

    Logger::debug(std::format("Brotli compressed: {} -> {} (via temp file)", input.string(), output.string()));
    return true;
}

bool Compressor::copy_metadata(const fs::path& source, const fs::path& dest) {
    struct stat st;
    
    // Получаем информацию об исходном файле (lstat для обработки ссылок)
    if (lstat(source.c_str(), &st) != 0) {
        Logger::warning(std::format("Failed to stat source file {}: {}", source.string(), strerror(errno)));
        return false;
    }
    
    // Открываем целевой файл через openat для защиты от symlink атак
    fs::path parent_dir = dest.parent_path();
    std::string basename = dest.filename().string();
    if (parent_dir.empty()) {
        parent_dir = ".";
    }
    
    int dir_fd = open(parent_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0) {
        Logger::warning(std::format("Failed to open directory {} for metadata copy: {}", parent_dir.string(), strerror(errno)));
        return false;
    }
    
    // Открываем файл без следования за symlink
    int dest_fd = openat(dir_fd, basename.c_str(), O_WRONLY | O_NOFOLLOW);
    close(dir_fd);
    
    if (dest_fd < 0) {
        Logger::warning(std::format("Failed to open destination file {} for metadata copy: {}", dest.string(), strerror(errno)));
        return false;
    }
    
    // 1. Копируем права доступа (режим) через fchmod
    if (fchmod(dest_fd, st.st_mode) != 0) {
        close(dest_fd);
        Logger::warning(std::format("Failed to set permissions on {}: {}", dest.string(), strerror(errno)));
        return false;
    }
    
    // 2. Копируем владельца и группу (только если запускаемся от root или имеем права)
    // fchown работает с fd, не следует за symlink
    if (fchown(dest_fd, st.st_uid, st.st_gid) != 0) {
        // Если не удалось сменить владельца (нет прав), логируем, но не считаем это фатальной ошибкой
        if (errno == EPERM) {
            Logger::debug(std::format("No permission to change ownership of {} (running as non-root?)", dest.string()));
        } else {
            close(dest_fd);
            Logger::warning(std::format("Failed to set ownership on {}: {}", dest.string(), strerror(errno)));
            return false;
        }
    }
    
    // 3. Копируем временные метки (atime, mtime) через futimens
    struct timespec times[2];
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    if (futimens(dest_fd, times) != 0) {
        close(dest_fd);
        Logger::warning(std::format("Failed to set timestamps on {}: {}", dest.string(), strerror(errno)));
        return false;
    }
    
    close(dest_fd);
    
    // 4. Копируем SELinux-контекст (опционально, если библиотека доступна)
    #ifdef HAVE_SELINUX
    char* src_context = nullptr;
    if (getfilecon(source.c_str(), &src_context) >= 0) {
        if (setfilecon(dest.c_str(), src_context) != 0) {
            Logger::debug(std::format("Failed to set SELinux context on {}: {}", dest.string(), strerror(errno)));
        }
        freecon(src_context);
    }
    #endif
    
    Logger::debug(std::format("Metadata copied: {} -> {} (mode={}, uid={}, gid={})", 
                              source.string(), dest.string(), st.st_mode, st.st_uid, st.st_gid));
    return true;
}

// Проверка: является ли путь символической ссылкой, указывающей за пределы разрешённых директорий
// Удалена небезопасная проверка с атомарным флагом - используется openat() для защиты от TOCTOU
bool Compressor::is_symlink_attack(const fs::path& path) {
    struct stat st;
    
    // Проверяем, является ли файл символической ссылкой
    if (lstat(path.c_str(), &st) != 0) {
        Logger::warning(std::format("Failed to lstat {}: {}", path.string(), strerror(errno)));
        return false;  // Не можем проверить - считаем безопасным для логирования
    }
    
    // Если это не symlink, атаки нет
    if (!S_ISLNK(st.st_mode)) {
        return false;
    }
    
    // Это symlink - проверяем, куда он указывает
    char target[PATH_MAX];
    ssize_t len = readlink(path.c_str(), target, sizeof(target) - 1);
    if (len < 0) {
        Logger::warning(std::format("Failed to readlink {}: {}", path.string(), strerror(errno)));
        return true;  // Считаем потенциальной атакой
    }
    target[len] = '\0';
    
    Logger::warning(std::format("Symlink detected: {} -> {}", path.string(), target));
    
    // Проверяем, указывает ли ссылка на файл вне типичных пользовательских директорий
    std::string target_str(target);
    if (target_str.find("/etc/") == 0 || 
        target_str.find("/usr/") == 0 || 
        target_str.find("/var/log/") == 0 ||
        target_str.find("/proc/") == 0 ||
        target_str.find("/sys/") == 0 ||
        target_str.find("/root/") == 0) {
        Logger::error(std::format("Potential symlink attack detected: {} points to system directory", path.string()));
        return true;
    }
    
    // Дополнительная проверка: resolving пути и проверка что он в допустимой зоне
    // Используем realpath для получения канонического пути
    char resolved_target[PATH_MAX];
    if (realpath(target, resolved_target) != nullptr) {
        std::string resolved_str(resolved_target);
        // Проверяем что целевой путь не ведет к системным файлам после разрешения symlink
        if (resolved_str.find("/etc/") == 0 || 
            resolved_str.find("/usr/") == 0 || 
            resolved_str.find("/var/log/") == 0 ||
            resolved_str.find("/proc/") == 0 ||
            resolved_str.find("/sys/") == 0 ||
            resolved_str.find("/root/") == 0) {
            Logger::error(std::format("Potential symlink attack detected: {} resolves to system file {}", path.string(), resolved_str));
            return true;
        }
    }
    
    return false;  // Symlink существует, но не указывает на системные директории
}

// Проверка владельца файла
bool Compressor::check_file_ownership(const fs::path& path, uid_t expected_uid) {
    struct stat st;
    
    if (lstat(path.c_str(), &st) != 0) {
        Logger::warning(std::format("Failed to stat {}: {}", path.string(), strerror(errno)));
        return false;
    }
    
    if (st.st_uid != expected_uid && expected_uid != 0) {
        // Получаем информацию о владельце для логирования
        struct passwd* pw = getpwuid(st.st_uid);
        const char* owner_name = pw ? pw->pw_name : "unknown";
        
        Logger::warning(std::format("File ownership mismatch for {}: expected uid={}, actual uid={} ({})", 
                                   path.string(), expected_uid, st.st_uid, owner_name));
        return false;
    }
    
    return true;
}

// Проверка пути: находится ли файл в разрешённой директории
// Улучшенная защита от обхода через symlink с использованием weakly_canonical для предотвращения атак с ".."
bool Compressor::validate_path_in_directory(const fs::path& path, const std::vector<std::string>& allowed_dirs) {
    // Используем weakly_canonical вместо canonical для защиты от атак с несуществующими путями
    // weakly_canonical разрешает существующую часть пути и аппроксимирует остальное
    std::error_code ec;
    fs::path resolved_path = fs::weakly_canonical(path, ec);
    
    if (ec || resolved_path.empty()) {
        Logger::warning(std::format("Failed to resolve path {}: {}", path.string(), ec.message()));
        return false;
    }
    
    // Проверяем наличие ".." в разрешённом пути после нормализации
    std::string resolved_str = resolved_path.string();
    if (resolved_str.find("..") != std::string::npos) {
        Logger::warning(std::format("Path traversal attempt detected: {} resolves to {}", path.string(), resolved_str));
        return false;
    }
    
    // Получаем родительскую директорию файла
    fs::path parent_dir = resolved_path.parent_path();
    std::string parent_str = parent_dir.string();
    
    // Проверяем, начинается ли канонический путь с одного из разрешённых
    for (const auto& allowed : allowed_dirs) {
        try {
            fs::path canonical_allowed = fs::canonical(allowed, ec);
            if (!ec) {
                std::string allowed_str = canonical_allowed.string();
                // Проверяем, что путь начинается с разрешённой директории
                if (parent_str.find(allowed_str) == 0) {
                    // Дополнительная проверка: убедимся, что это действительно поддиректория
                    // (например, /home/user должен соответствовать /home/user/file.txt,
                    // но не /home/users/file.txt)
                    if (parent_str.length() == allowed_str.length() ||
                        parent_str[allowed_str.length()] == '/') {
                        return true;
                    }
                }
            }
        } catch (const fs::filesystem_error& e) {
            Logger::debug(std::format("Cannot canonicalize allowed dir {}: {}", allowed, e.what()));
        }
    }
    
    Logger::warning(std::format("Path validation failed: {} is not in any allowed directory", resolved_str));
    return false;
}

// Безопасное удаление сжатых копий
bool Compressor::safe_remove_compressed(const fs::path& original_path) {
    std::error_code ec;
    
    // 1. Проверка на symlink-атаку для оригинального пути
    if (is_symlink_attack(original_path)) {
        Logger::error(std::format("Refusing to remove compressed copies: potential symlink attack for {}", original_path.string()));
        return false;
    }
    
    // 2. Получаем информацию об оригинальном файле (если он ещё существует)
    struct stat orig_st;
    bool orig_exists = (lstat(original_path.c_str(), &orig_st) == 0);
    uid_t expected_uid = orig_exists ? orig_st.st_uid : 0;  // 0 означает "не проверять владельца"
    
    if (!orig_exists) {
        // Файл уже удалён - используем uid=0 для пропуска проверки владельца
        Logger::debug(std::format("Original file does not exist, skipping ownership check for compressed copies of {}", original_path.string()));
    }
    
    // Вспомогательная функция для безопасного удаления файла
    auto safe_unlink = [](const fs::path& filepath, const char* type) -> bool {
        fs::path parent_dir = filepath.parent_path();
        std::string basename = filepath.filename().string();
        if (parent_dir.empty()) {
            parent_dir = ".";
        }
        
        // Открываем директорию с O_DIRECTORY | O_NOFOLLOW для защиты от symlink
        int dir_fd = open(parent_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (dir_fd < 0) {
            Logger::error(std::format("Failed to open directory {} for {} removal: {}", 
                                     parent_dir.string(), type, strerror(errno)));
            return false;
        }
        
        // Используем unlinkat с AT_REMOVEDIR для удаления без следования за symlink
        if (unlinkat(dir_fd, basename.c_str(), 0) != 0) {
            int saved_errno = errno;
            close(dir_fd);
            if (saved_errno == ENOENT) {
                Logger::debug(std::format("{} file does not exist: {}", type, filepath.string()));
                return true;  // Не ошибка, файл уже удалён
            }
            Logger::error(std::format("Failed to unlink {} {}: {}", type, filepath.string(), strerror(saved_errno)));
            return false;
        }
        
        close(dir_fd);
        Logger::info(std::format("Removed {} copy: {}", type, filepath.string()));
        return true;
    };
    
    // 3. Проверяем и удаляем .gz копию через lstat (безопасная проверка)
    fs::path gz_path = original_path.string() + ".gz";
    struct stat gz_st;
    if (lstat(gz_path.c_str(), &gz_st) == 0) {
        // Проверка на symlink-атаку для сжатого файла
        if (S_ISLNK(gz_st.st_mode)) {
            Logger::error(std::format("SECURITY: Refusing to remove {}: potential symlink attack", gz_path.string()));
        } else if (S_ISREG(gz_st.st_mode)) {
            // Проверка владельца (пропускаем, если оригинал не существует)
            if (!orig_exists || check_file_ownership(gz_path, expected_uid)) {
                safe_unlink(gz_path, "gzip");
            } else {
                Logger::error(std::format("Ownership check failed for {}, skipping removal", gz_path.string()));
            }
        }
    }
    
    // 4. Проверяем и удаляем .br копию через lstat (безопасная проверка)
    fs::path br_path = original_path.string() + ".br";
    struct stat br_st;
    if (lstat(br_path.c_str(), &br_st) == 0) {
        // Проверка на symlink-атаку для сжатого файла
        if (S_ISLNK(br_st.st_mode)) {
            Logger::error(std::format("SECURITY: Refusing to remove {}: potential symlink attack", br_path.string()));
        } else if (S_ISREG(br_st.st_mode)) {
            // Проверка владельца (пропускаем, если оригинал не существует)
            if (!orig_exists || check_file_ownership(br_path, expected_uid)) {
                safe_unlink(br_path, "brotli");
            } else {
                Logger::error(std::format("Ownership check failed for {}, skipping removal", br_path.string()));
            }
        }
    }
    
    return true;
}