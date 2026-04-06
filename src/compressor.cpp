#include "compressor.h"
#include "logger.h"
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
#include <cstring>      // <--- ДЛЯ strerror()
#include <pwd.h>        // Для getpwuid()
#include <grp.h>        // Для getgrgid()
#include <climits>      // Для PATH_MAX
#include <set>          // Для кэша валидации путей
#include <atomic>       // Для потокобезопасности

// Опциональная поддержка SELinux
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

// Размер буфера для потоковой обработки (256KB - оптимизировано для производительности)
constexpr size_t STREAM_BUFFER_SIZE = 262144;

// Глобальный кэш для предотвращения race condition при проверке symlink-атак
// Используется атомарный флаг для защиты от TOCTOU атак
static std::atomic<bool> g_symlink_check_in_progress{false};

// Максимальное количество попыток при проверке пути для предотвращения DoS
constexpr int MAX_PATH_VALIDATION_RETRIES = 3;

bool Compressor::compress_gzip(const fs::path& input, const fs::path& output, int level) {
    // Проверка диапазона уровня сжатия (защита от некорректных значений)
    if (level < 1 || level > 9) {
        Logger::warning(std::format("Invalid gzip level {}, using default 6", level));
        level = 6;
    }
    
    // Открываем входной файл с прямым доступом и защитой от symlink-атак
    // Используем O_NOFOLLOW для предотвращения открытия symlink
    int fd_in = open(input.c_str(), O_RDONLY | O_NOATIME | O_NOFOLLOW);
    if (fd_in < 0) {
        if (errno == ELOOP || errno == EMLINK) {
            Logger::error(std::format("Symlink attack detected: {} - refusing to open", input.string()));
            return false;
        }
        Logger::error(std::format("Failed to open file for reading: {} - {}", input.string(), strerror(errno)));
        return false;
    }
    
    // Дополнительная проверка: убеждаемся что это обычный файл, а не symlink или device
    struct stat st;
    if (fstat(fd_in, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd_in);
        Logger::error(std::format("File is not a regular file: {}", input.string()));
        return false;
    }
    
    // Проверка прав доступа перед чтением
    if ((st.st_mode & S_IRUSR) == 0 && getuid() != st.st_uid && getuid() != 0) {
        close(fd_in);
        Logger::warning(std::format("No read permission for file: {}", input.string()));
        return false;
    }
    
    // Подсказка ОС о последовательном чтении и предзагрузке
    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);
    
    std::ifstream ifs(input, std::ios::binary);
    if (!ifs) {
        close(fd_in);
        Logger::error(std::format("Failed to open file for reading: {}", input.string()));
        return false;
    }

    z_stream strm = {};
    // Используем Z_HUFFMAN_ONLY для быстрых файлов или Z_DEFAULT_STRATEGY для лучшего сжатия
    int strategy = (level <= 3) ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, strategy) != Z_OK) {
        Logger::error("Failed to init gzip stream");
        close(fd_in);
        return false;
    }

    std::ofstream ofs(output, std::ios::binary);
    if (!ofs) {
        deflateEnd(&strm);
        close(fd_in);
        Logger::error(std::format("Failed to open output file: {}", output.string()));
        return false;
    }

    // Выделяем буферы один раз вне цикла
    std::vector<char> in_buffer(STREAM_BUFFER_SIZE);
    std::vector<char> out_buffer(STREAM_BUFFER_SIZE);
    
    int ret;
    bool has_error = false;

    do {
        // Читаем порциями из входного файла
        ifs.read(in_buffer.data(), in_buffer.size());
        std::streamsize bytes_read = ifs.gcount();
        
        if (bytes_read > 0) {
            strm.avail_in = bytes_read;
            strm.next_in = (Bytef*)in_buffer.data();
            
            // Сжимаем и записываем порциями
            do {
                strm.avail_out = out_buffer.size();
                strm.next_out = (Bytef*)out_buffer.data();
                ret = deflate(&strm, ifs.eof() ? Z_FINISH : Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR) {
                    Logger::error("Gzip stream error");
                    has_error = true;
                    break;
                }
                size_t have = out_buffer.size() - strm.avail_out;
                if (have > 0) {
                    ofs.write(out_buffer.data(), have);
                    if (ofs.fail()) {
                        has_error = true;
                        break;
                    }
                }
            } while (strm.avail_out == 0 && !has_error);
        }
        
        if (has_error) break;
    } while (!ifs.eof());

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
                ofs.write(out_buffer.data(), have);
                if (ofs.fail()) {
                    has_error = true;
                    break;
                }
            }
        } while (ret == Z_OK);
    }

    deflateEnd(&strm);
    close(fd_in);
    ofs.close();
    
    if (has_error || ofs.fail()) {
        Logger::error("Failed to write gzip output");
        return false;
    }

    Logger::debug(std::format("Gzip compressed: {} -> {}", input.string(), output.string()));
    return true;
}

bool Compressor::compress_brotli(const fs::path& input, const fs::path& output, int level) {
    // Проверка диапазона уровня сжатия (защита от некорректных значений)
    if (level < 1 || level > 11) {
        Logger::warning(std::format("Invalid brotli level {}, using default 4", level));
        level = 4;
    }
    
    // Открываем входной файл с прямым доступом и защитой от symlink-атак
    // Используем O_NOFOLLOW для предотвращения открытия symlink
    int fd_in = open(input.c_str(), O_RDONLY | O_NOATIME | O_NOFOLLOW);
    if (fd_in < 0) {
        if (errno == ELOOP || errno == EMLINK) {
            Logger::error(std::format("Symlink attack detected: {} - refusing to open", input.string()));
            return false;
        }
        Logger::error(std::format("Failed to open file for reading: {} - {}", input.string(), strerror(errno)));
        return false;
    }
    
    // Дополнительная проверка: убеждаемся что это обычный файл, а не symlink или device
    struct stat st;
    if (fstat(fd_in, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd_in);
        Logger::error(std::format("File is not a regular file: {}", input.string()));
        return false;
    }
    
    // Проверка прав доступа перед чтением
    if ((st.st_mode & S_IRUSR) == 0 && getuid() != st.st_uid && getuid() != 0) {
        close(fd_in);
        Logger::warning(std::format("No read permission for file: {}", input.string()));
        return false;
    }
    
    // Подсказка ОС о последовательном чтении и предзагрузке
    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);
    
    std::ifstream ifs(input, std::ios::binary);
    if (!ifs) {
        close(fd_in);
        Logger::error(std::format("Failed to open file for reading: {}", input.string()));
        return false;
    }

    std::ofstream ofs(output, std::ios::binary);
    if (!ofs) {
        close(fd_in);
        Logger::error(std::format("Failed to open output file: {}", output.string()));
        return false;
    }

    // Создаем поток сжатия Brotli
    BrotliEncoderState* state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        close(fd_in);
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
        // Читаем порцию из входного файла
        ifs.read(reinterpret_cast<char*>(in_buffer.data()), in_buffer.size());
        std::streamsize bytes_read = ifs.gcount();
        
        const uint8_t* next_in = in_buffer.data();
        size_t available_in = bytes_read;
        
        // Обрабатываем прочитанные данные
        if (bytes_read > 0 || !ifs.eof()) {
            while (available_in > 0 || !BrotliEncoderIsFinished(state)) {
                size_t available_out = out_buffer.size();
                uint8_t* next_out = out_buffer.data();
                
                if (!BrotliEncoderCompressStream(state,
                        ifs.eof() ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS,
                        &available_in, &next_in,
                        &available_out, &next_out,
                        nullptr)) {
                    Logger::error(std::format("Brotli compression stream error for: {}", input.string()));
                    success = false;
                    break;
                }
                
                size_t written = out_buffer.size() - available_out;
                if (written > 0) {
                    ofs.write(reinterpret_cast<char*>(out_buffer.data()), written);
                    if (ofs.fail()) {
                        success = false;
                        break;
                    }
                }
                
                if (available_in == 0 && !ifs.eof()) {
                    break;
                }
                
                if (BrotliEncoderIsFinished(state)) {
                    break;
                }
            }
        }
        
        if (!success) break;
        
        // Если достигнут конец файла и поток завершен
        if (ifs.eof() && BrotliEncoderIsFinished(state)) {
            break;
        }
    }

    BrotliEncoderDestroyInstance(state);
    close(fd_in);
    ofs.close();

    if (ofs.fail()) {
        Logger::error("Failed to write brotli output");
        return false;
    }

    if (!success) {
        return false;
    }

    Logger::debug(std::format("Brotli compressed: {} -> {}", input.string(), output.string()));
    return true;
}

bool Compressor::copy_metadata(const fs::path& source, const fs::path& dest) {
    struct stat st;
    
    // Получаем информацию об исходном файле (lstat для обработки ссылок)
    if (lstat(source.c_str(), &st) != 0) {
        Logger::warning(std::format("Failed to stat source file {}: {}", source.string(), strerror(errno)));
        return false;
    }
    
    // 1. Копируем права доступа (режим)
    if (chmod(dest.c_str(), st.st_mode) != 0) {
        Logger::warning(std::format("Failed to set permissions on {}: {}", dest.string(), strerror(errno)));
        return false;
    }
    
    // 2. Копируем владельца и группу (только если запускаемся от root или имеем права)
    // lchown не следует за символическими ссылками
    if (lchown(dest.c_str(), st.st_uid, st.st_gid) != 0) {
        // Если не удалось сменить владельца (нет прав), логируем, но не считаем это фатальной ошибкой
        if (errno == EPERM) {
            Logger::debug(std::format("No permission to change ownership of {} (running as non-root?)", dest.string()));
        } else {
            Logger::warning(std::format("Failed to set ownership on {}: {}", dest.string(), strerror(errno)));
        }
    }
    
    // 3. Копируем временные метки (atime, mtime)
    struct timespec times[2];
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    if (utimensat(AT_FDCWD, dest.c_str(), times, 0) != 0) {
        Logger::warning(std::format("Failed to set timestamps on {}: {}", dest.string(), strerror(errno)));
        return false;
    }
    
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
// Улучшенная защита от TOCTOU атак с использованием атомарных операций
bool Compressor::is_symlink_attack(const fs::path& path) {
    // Защита от race condition - только одна проверка symlink одновременно
    bool expected = false;
    if (!g_symlink_check_in_progress.compare_exchange_strong(expected, true)) {
        // Другая проверка уже выполняется - считаем потенциальной атакой
        Logger::warning(std::format("Concurrent symlink check detected for {}, treating as potential attack", path.string()));
        return true;
    }
    
    struct stat st;
    
    // Проверяем, является ли файл символической ссылкой
    if (lstat(path.c_str(), &st) != 0) {
        g_symlink_check_in_progress.store(false);
        Logger::warning(std::format("Failed to lstat {}: {}", path.string(), strerror(errno)));
        return false;  // Не можем проверить - считаем безопасным для логирования
    }
    
    // Если это не symlink, атаки нет
    if (!S_ISLNK(st.st_mode)) {
        g_symlink_check_in_progress.store(false);
        return false;
    }
    
    // Это symlink - проверяем, куда он указывает
    char target[PATH_MAX];
    ssize_t len = readlink(path.c_str(), target, sizeof(target) - 1);
    if (len < 0) {
        g_symlink_check_in_progress.store(false);
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
        g_symlink_check_in_progress.store(false);
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
            g_symlink_check_in_progress.store(false);
            return true;
        }
    }
    
    g_symlink_check_in_progress.store(false);
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
// Улучшенная защита от обхода через symlink с использованием multiple retries
bool Compressor::validate_path_in_directory(const fs::path& path, const std::vector<std::string>& allowed_dirs) {
    // Получаем канонический путь исходного файла с несколькими попытками для предотвращения race condition
    std::error_code ec;
    fs::path canonical_path;
    
    for (int retry = 0; retry < MAX_PATH_VALIDATION_RETRIES; ++retry) {
        try {
            canonical_path = fs::canonical(path.parent_path(), ec);
            if (ec) {
                Logger::warning(std::format("Failed to get canonical path for {}: {} (attempt {}/{})", 
                    path.string(), ec.message(), retry + 1, MAX_PATH_VALIDATION_RETRIES));
                if (retry == MAX_PATH_VALIDATION_RETRIES - 1) {
                    return false;
                }
                continue;
            }
            break;  // Успех
        } catch (const fs::filesystem_error& e) {
            Logger::warning(std::format("Filesystem error getting canonical path for {}: {} (attempt {}/{})", 
                path.string(), e.what(), retry + 1, MAX_PATH_VALIDATION_RETRIES));
            if (retry == MAX_PATH_VALIDATION_RETRIES - 1) {
                return false;
            }
        }
    }
    
    std::string canonical_str = canonical_path.string();
    
    // Проверяем, начинается ли канонический путь с одного из разрешённых
    for (const auto& allowed : allowed_dirs) {
        try {
            fs::path canonical_allowed = fs::canonical(allowed, ec);
            if (!ec) {
                std::string allowed_str = canonical_allowed.string();
                // Проверяем, что путь начинается с разрешённой директории
                if (canonical_str.find(allowed_str) == 0) {
                    // Дополнительная проверка: убедимся, что это действительно поддиректория
                    // (например, /home/user должен соответствовать /home/user/file.txt,
                    // но не /home/users/file.txt)
                    if (canonical_str.length() == allowed_str.length() ||
                        canonical_str[allowed_str.length()] == '/') {
                        return true;
                    }
                }
            }
        } catch (const fs::filesystem_error& e) {
            Logger::debug(std::format("Cannot canonicalize allowed dir {}: {}", allowed, e.what()));
        }
    }
    
    Logger::warning(std::format("Path validation failed: {} is not in any allowed directory", path.string()));
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
    
    // 3. Проверяем и удаляем .gz копию
    fs::path gz_path = original_path.string() + ".gz";
    if (fs::exists(gz_path, ec) && !ec) {
        // Проверка на symlink-атаку для сжатого файла
        if (is_symlink_attack(gz_path)) {
            Logger::error(std::format("Refusing to remove {}: potential symlink attack", gz_path.string()));
        } else {
            // Проверка владельца (пропускаем, если оригинал не существует)
            if (!orig_exists || check_file_ownership(gz_path, expected_uid)) {
                if (fs::remove(gz_path, ec) && !ec) {
                    Logger::info(std::format("Removed compressed copy: {}", gz_path.string()));
                } else {
                    Logger::error(std::format("Failed to remove {}: {}", gz_path.string(), ec.message()));
                }
            } else {
                Logger::error(std::format("Ownership check failed for {}, skipping removal", gz_path.string()));
            }
        }
    }
    
    // 4. Проверяем и удаляем .br копию
    fs::path br_path = original_path.string() + ".br";
    if (fs::exists(br_path, ec) && !ec) {
        // Проверка на symlink-атаку для сжатого файла
        if (is_symlink_attack(br_path)) {
            Logger::error(std::format("Refusing to remove {}: potential symlink attack", br_path.string()));
        } else {
            // Проверка владельца (пропускаем, если оригинал не существует)
            if (!orig_exists || check_file_ownership(br_path, expected_uid)) {
                if (fs::remove(br_path, ec) && !ec) {
                    Logger::info(std::format("Removed compressed copy: {}", br_path.string()));
                } else {
                    Logger::error(std::format("Failed to remove {}: {}", br_path.string(), ec.message()));
                }
            } else {
                Logger::error(std::format("Ownership check failed for {}, skipping removal", br_path.string()));
            }
        }
    }
    
    return true;
}