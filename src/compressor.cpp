#include "compressor.h"
#include "logger.h"
#include "memory_pool.h"
#include "async_io.h"
#include "performance_optimizer.h"
#include "i18n.h"
#ifdef HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif
#include <vector>
#include <zlib.h>
#include <brotli/encode.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
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
#include <atomic>       // Для потокобезопасности
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

// Базовый размер буфера для потоковой обработки (1MB - оптимизировано для производительности)
constexpr size_t BASE_BUFFER_SIZE = 1048576;

// Минимальный размер буфера для маленьких файлов (64KB)
constexpr size_t MIN_BUFFER_SIZE = 65536;

// Максимальный размер буфера (4MB)
constexpr size_t MAX_BUFFER_SIZE = 4194304;

// Порог размера файла для выбора размера буфера
constexpr uint64_t SMALL_FILE_THRESHOLD = 256 * 1024;      // 256KB
constexpr uint64_t MEDIUM_FILE_THRESHOLD = 4 * 1024 * 1024; // 4MB
constexpr uint64_t LARGE_FILE_THRESHOLD = 16 * 1024 * 1024; // 16MB

// Максимальное количество попыток при проверке пути для предотвращения DoS
constexpr int MAX_PATH_VALIDATION_RETRIES = 3;

// Буфер для /proc/self/fd/<fd> путей (достаточно для большинства систем)
// Размер буфера для пути /proc/self/fd/NNN (достаточно для больших PID)
// Формат: "/proc/self/fd/" + номер FD (до 10 цифр) + '\0' = ~25 байт
// Увеличено до 128 для запаса и защиты от переполнения
constexpr size_t PROC_FD_PATH_SIZE = 128;

// Максимальный размер файла для сжатия (100MB) - защита от DoS атак
constexpr uint64_t MAX_FILE_SIZE = 100 * 1024 * 1024;

// Пул буферов создаётся при первом сжатии (lazy), а не при старте процесса.
// Иначе до main() выполнялась тяжёлая инициализация (mmap / NUMA), и даже
// «mh-compressor-manager --help» зависал на заметное время.
// Сделана публичной для использования из main.cpp (однократное чтение, ТЗ §3.2.4)
ByteBufferPool& buffer_pool() {
    static ByteBufferPool pool(16, -1, ByteBufferPool::MAX_POOL_SIZE);
    return pool;
}

// Счётчик для периодического освобождения буферов пула
std::atomic<size_t>& pool_shrink_counter() {
    static std::atomic<size_t> counter{0};
    return counter;
}

// Выравнивание для O_DIRECT (должно быть кратно размеру сектора, обычно 512 байт)
constexpr size_t DIRECT_IO_ALIGNMENT = 4096;

/**
 * Вычисляет оптимальный размер буфера на основе размера файла
 * Адаптивный подход улучшает производительность:
 * - Маленькие файлы: меньшие буферы для экономии памяти
 * - Большие файлы: большие буферы для уменьшения количества системных вызовов
 */
inline size_t calculate_buffer_size(uint64_t file_size) {
    if (file_size <= SMALL_FILE_THRESHOLD) {
        return MIN_BUFFER_SIZE;
    } else if (file_size <= MEDIUM_FILE_THRESHOLD) {
        return BASE_BUFFER_SIZE / 2;
    } else if (file_size <= LARGE_FILE_THRESHOLD) {
        return BASE_BUFFER_SIZE;
    } else {
        return MAX_BUFFER_SIZE;
    }
}

/**
 * Выравнивает размер по границе для O_DIRECT
 */
inline size_t align_for_direct_io(size_t size) {
    return (size + DIRECT_IO_ALIGNMENT - 1) & ~(DIRECT_IO_ALIGNMENT - 1);
}

// ============================================================================
// Однократное чтение: сжатие из буфера в памяти (ТЗ §3.2.4)
// ============================================================================

bool Compressor::copy_metadata(const fs::path& source, const fs::path& dest) {
    struct stat st;
    
    // Получаем информацию об исходном файле (lstat для обработки ссылок)
    if (lstat(source.c_str(), &st) != 0) {
        Logger::warning(_("Failed to stat source file %s: %s"), source.string().c_str(), strerror(errno));
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
        Logger::warning(_("Failed to open directory %s for metadata copy: %s"), parent_dir.string().c_str(), strerror(errno));
        return false;
    }
    
    // Открываем файл без следования за symlink
    int dest_fd = openat(dir_fd, basename.c_str(), O_WRONLY | O_NOFOLLOW);
    close(dir_fd);
    
    if (dest_fd < 0) {
        Logger::warning(_("Failed to open destination file %s for metadata copy: %s"), dest.string().c_str(), strerror(errno));
        return false;
    }
    
    // 1. Копируем права доступа (режим) через fchmod
    if (fchmod(dest_fd, st.st_mode) != 0) {
        close(dest_fd);
        Logger::warning(_("Failed to set permissions on %s: %s"), dest.string().c_str(), strerror(errno));
        return false;
    }
    
    // 2. Копируем владельца и группу (только если запускаемся от root или имеем права)
    // fchown работает с fd, не следует за symlink
    if (fchown(dest_fd, st.st_uid, st.st_gid) != 0) {
        // Если не удалось сменить владельца (нет прав), логируем, но не считаем это фатальной ошибкой
        if (errno == EPERM) {
            Logger::debug(_("No permission to change ownership of %s (running as non-root?)"), dest.string().c_str());
        } else {
            close(dest_fd);
            Logger::warning(_("Failed to set ownership on %s: %s"), dest.string().c_str(), strerror(errno));
            return false;
        }
    }
    
    // 3. Копируем временные метки (atime, mtime) через futimens
    struct timespec times[2];
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    if (futimens(dest_fd, times) != 0) {
        close(dest_fd);
        Logger::warning(_("Failed to set timestamps on %s: %s"), dest.string().c_str(), strerror(errno));
        return false;
    }
    
    close(dest_fd);
    
    // 4. Копируем SELinux-контекст (опционально, если библиотека доступна)
    #ifdef HAVE_SELINUX
    char* src_context = nullptr;
    if (getfilecon(source.c_str(), &src_context) >= 0) {
        if (setfilecon(dest.c_str(), src_context) != 0) {
            Logger::debug(_("Failed to set SELinux context on %s: %s"), dest.string().c_str(), strerror(errno));
        }
        freecon(src_context);
    }
    #endif

    Logger::debug(_("Metadata copied: %s -> %s (mode=%u, uid=%u, gid=%u)"),
                              source.string().c_str(), dest.string().c_str(), st.st_mode, st.st_uid, st.st_gid);
    return true;
}

// Проверка: является ли путь символической ссылкой.
// Отклоняем ВСЕ symlink — не только указывающие на системные пути (ТЗ §8.4).
bool Compressor::is_symlink_attack(const fs::path& path) {
    struct stat st;

    // Проверяем, является ли файл символической ссылкой
    if (lstat(path.c_str(), &st) != 0) {
        Logger::warning(_("Failed to lstat %s: %s"), path.string().c_str(), strerror(errno));
        return false;  // Не можем проверить - считаем безопасным для логирования
    }

    // Любой symlink отклоняется — защита от symlink-атак (ТЗ §8.4)
    if (S_ISLNK(st.st_mode)) {
        Logger::error(_("Refusing to process symlink: %s"), path.string().c_str());
        return true;  // Атака — symlink запрещён
    }

    return false;  // Не symlink — безопасно
}

// Проверка владельца файла
bool Compressor::check_file_ownership(const fs::path& path, uid_t expected_uid) {
    struct stat st;
    
    if (lstat(path.c_str(), &st) != 0) {
        Logger::warning(_("Failed to stat %s: %s"), path.string().c_str(), strerror(errno));
        return false;
    }

    if (st.st_uid != expected_uid && expected_uid != 0) {
        // Получаем информацию о владельце для логирования
        struct passwd* pw = getpwuid(st.st_uid);
        const char* owner_name = pw ? pw->pw_name : "unknown";

        Logger::warning(_("File ownership mismatch for %s: expected uid=%u, actual uid=%u (%s)"),
                                   path.string().c_str(), expected_uid, st.st_uid, owner_name);
        return false;
    }
    
    return true;
}

// Безопасное удаление сжатых копий
bool Compressor::safe_remove_compressed(const fs::path& original_path) {
    std::error_code ec;
    
    // 1. Проверка на symlink-атаку для оригинального пути
    if (is_symlink_attack(original_path)) {
        Logger::error(_("Refusing to remove compressed copies: potential symlink attack for %s"), original_path.string().c_str());
        return false;
    }
    
    // 2. Получаем информацию об оригинальном файле (если он ещё существует)
    struct stat orig_st;
    bool orig_exists = (lstat(original_path.c_str(), &orig_st) == 0);
    uid_t expected_uid = orig_exists ? orig_st.st_uid : 0;  // 0 означает "не проверять владельца"
    
    if (!orig_exists) {
        // Файл уже удалён - используем uid=0 для пропуска проверки владельца
        Logger::debug(_("Original file does not exist, skipping ownership check for compressed copies of %s"), original_path.string().c_str());
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
            Logger::error(_("Failed to open directory %s for %s removal: %s"),
                                     parent_dir.string().c_str(), type, strerror(errno));
            return false;
        }

        // Используем unlinkat с AT_REMOVEDIR для удаления без следования за symlink
        if (unlinkat(dir_fd, basename.c_str(), 0) != 0) {
            int saved_errno = errno;
            close(dir_fd);
            if (saved_errno == ENOENT) {
                Logger::debug(_("%s file does not exist: %s"), type, filepath.string().c_str());
                return true;  // Не ошибка, файл уже удалён
            }
            Logger::error(_("Failed to unlink %s %s: %s"), type, filepath.string().c_str(), strerror(saved_errno));
            return false;
        }

        close(dir_fd);
        Logger::info(_("Removed %s copy: %s"), type, filepath.string().c_str());
        return true;
    };
    
    // 3. Проверяем и удаляем .gz копию через lstat (безопасная проверка)
    fs::path gz_path = original_path.string() + ".gz";
    struct stat gz_st;
    if (lstat(gz_path.c_str(), &gz_st) == 0) {
        // Проверка на symlink-атаку для сжатого файла
        if (S_ISLNK(gz_st.st_mode)) {
            Logger::error(_("SECURITY: Refusing to remove %s: potential symlink attack"), gz_path.string().c_str());
        } else if (S_ISREG(gz_st.st_mode)) {
            // Проверка владельца (пропускаем, если оригинал не существует)
            if (!orig_exists || check_file_ownership(gz_path, expected_uid)) {
                safe_unlink(gz_path, "gzip");
            } else {
                Logger::error(_("Ownership check failed for %s, skipping removal"), gz_path.string().c_str());
            }
        }
    }

    // 4. Проверяем и удаляем .br копию через lstat (безопасная проверка)
    fs::path br_path = original_path.string() + ".br";
    struct stat br_st;
    if (lstat(br_path.c_str(), &br_st) == 0) {
        // Проверка на symlink-атаку для сжатого файла
        if (S_ISLNK(br_st.st_mode)) {
            Logger::error(_("SECURITY: Refusing to remove %s: potential symlink attack"), br_path.string().c_str());
        } else if (S_ISREG(br_st.st_mode)) {
            // Проверка владельца (пропускаем, если оригинал не существует)
            if (!orig_exists || check_file_ownership(br_path, expected_uid)) {
                safe_unlink(br_path, "brotli");
            } else {
                Logger::error(_("Ownership check failed for %s, skipping removal"), br_path.string().c_str());
            }
        }
    }

    return true;
}

// ============================================================================
// Однократное чтение: сжатие из буфера в памяти (ТЗ §3.2.4)
// ============================================================================

/**
 * @brief Атомарная запись в файл через временный файл + rename()
 * Используется функциями compress_*_from_memory
 */
namespace {
bool write_atomic_file(const fs::path& output_path, const uint8_t* data, size_t size, mode_t mode) {
    // Создаём временный файл
    std::string tmp_path_str = output_path.string() + ".tmp";
    fs::path tmp_path = tmp_path_str;

    int fd_out = open(tmp_path_str.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd_out < 0) {
        int saved_errno = errno;
        Logger::error(_("Failed to open temp file %s: %s"), tmp_path_str.c_str(), strerror(saved_errno));
        return false;
    }

    // Записываем все данные
    size_t total_written = 0;
    while (total_written < size) {
        ssize_t written = write(fd_out, data + total_written, size - total_written);
        if (written < 0) {
            if (errno == EINTR) continue;
            int saved_errno = errno;
            close(fd_out);
            unlink(tmp_path_str.c_str());
            Logger::error(_("Failed to write temp file %s: %s"), tmp_path_str.c_str(), strerror(saved_errno));
            return false;
        }
        total_written += written;
    }

    // Синхронизируем и закрываем
    if (fsync(fd_out) != 0) {
        int saved_errno = errno;
        close(fd_out);
        unlink(tmp_path_str.c_str());
        Logger::error(_("fsync failed for %s: %s"), tmp_path_str.c_str(), strerror(saved_errno));
        return false;
    }
    close(fd_out);

    // Атомарное переименование
    if (rename(tmp_path_str.c_str(), output_path.c_str()) != 0) {
        int saved_errno = errno;
        unlink(tmp_path_str.c_str());
        Logger::error(_("rename failed %s -> %s: %s"), tmp_path_str.c_str(), output_path.string().c_str(), strerror(saved_errno));
        return false;
    }

    return true;
}
}  // namespace

bool Compressor::compress_gzip_from_memory(const uint8_t* data, size_t size,
                                            const fs::path& output_path, int level) {
    if (!data || size == 0) {
        Logger::error(_("compress_gzip_from_memory: null data or zero size"));
        return false;
    }

    // Проверка диапазона уровня сжатия
    if (!validate_compression_level(level, 1, 9)) {
        Logger::warning(_("Invalid gzip level %d, using default 6"), level);
        level = 6;
    }

    // Определяем стратегию: libdeflate (2x быстрее) или zlib
#ifdef HAVE_LIBDEFLATE
    return compress_gzip_libdeflate_from_memory(data, size, output_path, level);
#else
    return compress_gzip_zlib_from_memory(data, size, output_path, level);
#endif
}

bool Compressor::compress_brotli_from_memory(const uint8_t* data, size_t size,
                                              const fs::path& output_path, int level) {
    if (!data || size == 0) {
        Logger::error(_("compress_brotli_from_memory: null data or zero size"));
        return false;
    }

    // Проверка диапазона уровня сжатия
    if (!validate_compression_level(level, 1, 11)) {
        Logger::warning(_("Invalid brotli level %d, using default 4"), level);
        level = 4;
    }

    // Создаём кодировщик Brotli
    BrotliEncoderState* state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        Logger::error(_("Failed to create Brotli encoder"));
        return false;
    }

    BrotliEncoderSetParameter(state, BROTLI_PARAM_QUALITY, (uint32_t)level);
    BrotliEncoderSetParameter(state, BROTLI_PARAM_MODE, BROTLI_MODE_TEXT);

    // Выделяем буфер вывода
    size_t max_compressed_size = BrotliEncoderMaxCompressedSize(size);
    std::vector<uint8_t> compressed(max_compressed_size);

    const uint8_t* next_in = data;
    size_t available_in = size;
    uint8_t* next_out = compressed.data();
    size_t available_out = max_compressed_size;

    if (!BrotliEncoderCompressStream(state, BROTLI_OPERATION_FINISH,
                                      &available_in, &next_in,
                                      &available_out, &next_out,
                                      nullptr)) {
        Logger::error(_("Brotli compression stream error for output: %s"), output_path.string().c_str());
        BrotliEncoderDestroyInstance(state);
        return false;
    }

    size_t compressed_size = max_compressed_size - available_out;
    BrotliEncoderDestroyInstance(state);

    if (compressed_size == 0) {
        Logger::error(_("Brotli produced zero bytes for output: %s"), output_path.string().c_str());
        return false;
    }

    // Атомарная запись
    if (!write_atomic_file(output_path, compressed.data(), compressed_size, 0644)) {
        return false;
    }

    Logger::debug(_("Brotli compressed from memory: %zu bytes -> %zu bytes -> %s"),
                               size, compressed_size, output_path.string().c_str());
    return true;
}

// ============================================================================
// zlib backend для compress_*_from_memory (fallback если libdeflate недоступен)
// ============================================================================

bool Compressor::compress_gzip_zlib_from_memory(const uint8_t* data, size_t size,
                                                  const fs::path& output_path, int level) {
    // Проверка диапазона
    if (!validate_compression_level(level, 1, 9)) {
        level = 6;
    }

    // Определяем стратегию сжатия
    int strategy = (level <= 3) ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;

    z_stream strm = {};
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, strategy) != Z_OK) {
        Logger::error(_("Failed to init gzip stream (zlib)"));
        return false;
    }

    // Выделяем буфер вывода
    size_t max_compressed_size = compressBound(size);
    std::vector<uint8_t> compressed(max_compressed_size);

    strm.avail_in = size;
    strm.next_in = const_cast<uint8_t*>(data);  // zlib требует не-const, но не модифицирует
    strm.avail_out = max_compressed_size;
    strm.next_out = compressed.data();

    // Сжимаем всё за один проход (one-shot для данных в памяти)
    int ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        Logger::error(_("Gzip deflate error (zlib), ret=%d"), ret);
        deflateEnd(&strm);
        return false;
    }

    size_t compressed_size = max_compressed_size - strm.avail_out;
    deflateEnd(&strm);

    if (compressed_size == 0) {
        Logger::error(_("Gzip produced zero bytes (zlib) for output: %s"), output_path.string().c_str());
        return false;
    }

    // Атомарная запись
    if (!write_atomic_file(output_path, compressed.data(), compressed_size, 0644)) {
        return false;
    }

    Logger::debug(_("Gzip compressed from memory (zlib): %zu bytes -> %zu bytes -> %s"),
                               size, compressed_size, output_path.string().c_str());
    return true;
}

// ============================================================================
// libdeflate backend для compress_gzip_from_memory (ТЗ §3.2.7, 2x быстрее zlib)
// ============================================================================

#ifdef HAVE_LIBDEFLATE
bool Compressor::compress_gzip_libdeflate_from_memory(const uint8_t* data, size_t size,
                                                       const fs::path& output_path, int level) {
    // Создаём компрессор ДО вычисления bound (ТЗ §3.2.7 — исправление UB)
    struct libdeflate_compressor* comp = libdeflate_alloc_compressor(level);
    if (!comp) {
        Logger::error(_("libdeflate_alloc_compressor failed for %s"), output_path.string().c_str());
        return false;
    }

    // Определяем максимальный размер сжатых данных (корректно: с compressor*, не nullptr)
    size_t max_out_size = libdeflate_gzip_compress_bound(comp, size);
    std::vector<uint8_t> compressed(max_out_size);

    // Сжимаем
    size_t actual_out_size = libdeflate_gzip_compress(comp, data, size, compressed.data(), max_out_size);
    libdeflate_free_compressor(comp);

    if (actual_out_size == 0) {
        Logger::error(_("libdeflate_gzip_compress failed for %s"), output_path.string().c_str());
        return false;
    }

    // Атомарная запись
    if (!write_atomic_file(output_path, compressed.data(), actual_out_size, 0644)) {
        return false;
    }

    Logger::debug(_("Gzip compressed from memory (libdeflate): %zu bytes -> %zu bytes -> %s"),
                               size, actual_out_size, output_path.string().c_str());
    return true;
}
#endif  // HAVE_LIBDEFLATE

// ============================================================================
// Streaming API для чанкового сжатия (ТЗ §3.2.9, §21.3, §21.3-Задача 2.3)
// ============================================================================

// Деструкторы
Compressor::GzipStreamState::~GzipStreamState() {
    if (strm) {
        deflateEnd(strm);
        delete strm;
        strm = nullptr;
    }
    if (fd_out >= 0) {
        close(fd_out);
        fd_out = -1;
    }
    if (!tmp_path.empty()) {
        unlink(tmp_path.c_str());
    }
}

Compressor::BrotliStreamState::~BrotliStreamState() {
    if (enc) {
        BrotliEncoderDestroyInstance(enc);
        enc = nullptr;
    }
    if (fd_out >= 0) {
        close(fd_out);
        fd_out = -1;
    }
    if (!tmp_path.empty()) {
        unlink(tmp_path.c_str());
    }
}

// GZIP streaming
bool Compressor::gzip_stream_start(GzipStreamState& state, int level, const fs::path& output_path, mode_t src_mode) {
    state.tmp_path = output_path.string() + ".tmp";
    state.final_path = output_path.string();
    state.has_error = false;

    // Удаляем stale временный файл если остался с прошлого раза
    unlink(state.tmp_path.c_str());

    // Открываем временный файл
    state.fd_out = open(state.tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, src_mode & 0666);
    if (state.fd_out < 0) {
        Logger::error(_("gzip_stream_start: failed to open temp file %s: %s"), state.tmp_path.c_str(), strerror(errno));
        state.has_error = true;
        return false;
    }

    // Инициализируем deflate
    state.strm = new z_stream();
    memset(state.strm, 0, sizeof(z_stream));

    int strategy = (level <= 3) ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
    if (deflateInit2(state.strm, level, Z_DEFLATED, 15 + 16, 8, strategy) != Z_OK) {
        Logger::error(_("gzip_stream_start: deflateInit2 failed"));
        state.has_error = true;
        return false;
    }

    state.initialized = true;
    Logger::debug(_("gzip_stream_start: %s"), output_path.string().c_str());
    return true;
}

bool Compressor::gzip_stream_process(GzipStreamState& state, const uint8_t* data, size_t size, bool flush) {
    if (state.has_error || !state.initialized || !state.strm) {
        return false;
    }

    // Буфер вывода — 128 КБ (достаточно для большинства чанков)
    constexpr size_t OUT_BUF_SIZE = 128 * 1024;
    std::vector<uint8_t> out_buf(OUT_BUF_SIZE);

    state.strm->avail_in = size;
    state.strm->next_in = const_cast<uint8_t*>(data);

    // Основной цикл: сжатие данных
    int last_ret = Z_OK;
    do {
        state.strm->avail_out = OUT_BUF_SIZE;
        state.strm->next_out = out_buf.data();

        last_ret = deflate(state.strm, flush ? Z_FINISH : Z_NO_FLUSH);
        if (last_ret == Z_STREAM_ERROR) {
            Logger::error(_("gzip_stream_process: Z_STREAM_ERROR"));
            state.has_error = true;
            return false;
        }

        size_t have = OUT_BUF_SIZE - state.strm->avail_out;
        if (have > 0) {
            size_t total_written = 0;
            while (total_written < have) {
                ssize_t n = write(state.fd_out, out_buf.data() + total_written, have - total_written);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    Logger::error(_("gzip_stream_process: write failed: %s"), strerror(errno));
                    state.has_error = true;
                    return false;
                }
                total_written += n;
            }
        }

        if (last_ret == Z_STREAM_END) break;
    } while (state.strm->avail_out == 0);

    // Отдельный цикл финализации при flush — Z_FINISH может требовать много вызовов.
    // Пропускаем если первый цикл уже завершил stream (Z_STREAM_END) — иначе
    // повторный deflate(Z_FINISH) на завершённом stream вернёт Z_STREAM_ERROR.
    if (flush && last_ret != Z_STREAM_END) {
        while (true) {
            state.strm->avail_out = OUT_BUF_SIZE;
            state.strm->next_out = out_buf.data();
            int ret = deflate(state.strm, Z_FINISH);
            if (ret == Z_STREAM_ERROR) {
                Logger::error(_("gzip_stream_process: Z_STREAM_ERROR in finalize"));
                state.has_error = true;
                return false;
            }

            size_t have = OUT_BUF_SIZE - state.strm->avail_out;
            if (have > 0) {
                size_t total_written = 0;
                while (total_written < have) {
                    ssize_t n = write(state.fd_out, out_buf.data() + total_written, have - total_written);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        Logger::error(_("gzip_stream_process: write failed in finalize: %s"), strerror(errno));
                        state.has_error = true;
                        return false;
                    }
                    total_written += n;
                }
            }

            if (ret == Z_STREAM_END) break;
        }
    }

    // Финализация: fsync, close, rename — выполняется при flush независимо от того,
    // завершил ли deflate в первом или втором цикле
    if (flush) {
        if (fsync(state.fd_out) != 0) {
            Logger::error(_("gzip_stream_process: fsync failed: %s"), strerror(errno));
            state.has_error = true;
            return false;
        }
        close(state.fd_out);
        state.fd_out = -1;

        if (rename(state.tmp_path.c_str(), state.final_path.c_str()) != 0) {
            Logger::error(_("gzip_stream_process: rename failed: %s -> %s (errno: %s)"), state.tmp_path.c_str(), state.final_path.c_str(), strerror(errno));
            state.has_error = true;
            return false;
        }
        state.tmp_path.clear();  // Больше не нужно удалять временный файл
        Logger::debug(_("gzip_stream_process: finished, renamed to %s"), state.final_path.c_str());
    }

    return true;
}

// BROTLI streaming
bool Compressor::brotli_stream_start(BrotliStreamState& state, int level, const fs::path& output_path, mode_t src_mode) {
    state.tmp_path = output_path.string() + ".tmp";
    state.final_path = output_path.string();
    state.has_error = false;

    // Удаляем stale временный файл если остался с прошлого раза
    unlink(state.tmp_path.c_str());

    // Открываем временный файл
    state.fd_out = open(state.tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, src_mode & 0666);
    if (state.fd_out < 0) {
        Logger::error(_("brotli_stream_start: failed to open temp file %s: %s"), state.tmp_path.c_str(), strerror(errno));
        state.has_error = true;
        return false;
    }

    // Создаём encoder
    state.enc = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state.enc) {
        Logger::error(_("brotli_stream_start: BrotliEncoderCreateInstance failed"));
        state.has_error = true;
        return false;
    }

    BrotliEncoderSetParameter(state.enc, BROTLI_PARAM_QUALITY, (uint32_t)level);
    BrotliEncoderSetParameter(state.enc, BROTLI_PARAM_MODE, BROTLI_MODE_TEXT);

    state.initialized = true;
    Logger::debug(_("brotli_stream_start: %s"), output_path.string().c_str());
    return true;
}

bool Compressor::brotli_stream_process(BrotliStreamState& state, const uint8_t* data, size_t size, bool flush) {
    if (state.has_error || !state.initialized || !state.enc) {
        return false;
    }

    // Защита от двойного flush — encoder уже финализирован
    if (flush && state.finalized) {
        Logger::warning(_("brotli_stream_process: flush called twice, ignoring"));
        return false;
    }
    if (flush) state.finalized = true;

    constexpr size_t OUT_BUF_SIZE = 128 * 1024;
    std::vector<uint8_t> out_buf(OUT_BUF_SIZE);

    const uint8_t* next_in = data;
    size_t available_in = size;

    BrotliEncoderOperation op = flush ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;

    while (available_in > 0 || !BrotliEncoderIsFinished(state.enc)) {
        size_t available_out = OUT_BUF_SIZE;
        uint8_t* next_out = out_buf.data();

        if (!BrotliEncoderCompressStream(state.enc, op,
                                          &available_in, &next_in,
                                          &available_out, &next_out,
                                          nullptr)) {
            Logger::error(_("brotli_stream_process: BrotliEncoderCompressStream failed"));
            state.has_error = true;
            return false;
        }

        size_t have = OUT_BUF_SIZE - available_out;
        if (have > 0) {
            size_t total_written = 0;
            while (total_written < have) {
                ssize_t n = write(state.fd_out, out_buf.data() + total_written, have - total_written);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    Logger::error(_("brotli_stream_process: write failed: %s"), strerror(errno));
                    state.has_error = true;
                    return false;
                }
                total_written += n;
            }
        }

        if (available_in == 0 && !flush) break;
        if (BrotliEncoderIsFinished(state.enc)) break;
    }

    if (flush) {
        // Убеждаемся что encoder полностью завершил
        while (!BrotliEncoderIsFinished(state.enc)) {
            size_t available_out = OUT_BUF_SIZE;
            uint8_t* next_out = out_buf.data();

            if (!BrotliEncoderCompressStream(state.enc, BROTLI_OPERATION_FINISH,
                                              nullptr, nullptr,
                                              &available_out, &next_out,
                                              nullptr)) {
                state.has_error = true;
                return false;
            }

            size_t have = OUT_BUF_SIZE - available_out;
            if (have > 0) {
                size_t total_written = 0;
                while (total_written < have) {
                    ssize_t n = write(state.fd_out, out_buf.data() + total_written, have - total_written);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        state.has_error = true;
                        return false;
                    }
                    total_written += n;
                }
            }
        }

        if (fsync(state.fd_out) != 0) {
            Logger::error(_("brotli_stream_process: fsync failed: %s"), strerror(errno));
            state.has_error = true;
            return false;
        }
        close(state.fd_out);
        state.fd_out = -1;

        if (rename(state.tmp_path.c_str(), state.final_path.c_str()) != 0) {
            Logger::error(_("brotli_stream_process: rename failed: %s -> %s (errno: %s)"), state.tmp_path.c_str(), state.final_path.c_str(), strerror(errno));
            state.has_error = true;
            return false;
        }
        state.tmp_path.clear();
        Logger::debug(_("brotli_stream_process: finished, renamed to %s"), state.final_path.c_str());
    }

    return true;
}
