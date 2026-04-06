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
#include <cstring>      // <--- ДОБАВЛЕНО: для strerror()

// Опциональная поддержка SELinux
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

// Размер буфера для потоковой обработки (128KB - увеличено для лучшей производительности)
constexpr size_t STREAM_BUFFER_SIZE = 131072;

bool Compressor::compress_gzip(const fs::path& input, const fs::path& output, int level) {
    // Открываем входной файл с прямым доступом
    int fd_in = open(input.c_str(), O_RDONLY);
    if (fd_in < 0) {
        Logger::error(std::format("Failed to open file for reading: {} - {}", input.string(), strerror(errno)));
        return false;
    }
    
    // Подсказка ОС о последовательном чтении
    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);
    
    std::ifstream ifs(input, std::ios::binary);
    if (!ifs) {
        close(fd_in);
        Logger::error(std::format("Failed to open file for reading: {}", input.string()));
        return false;
    }

    z_stream strm = {};
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
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
    // Открываем входной файл с прямым доступом
    int fd_in = open(input.c_str(), O_RDONLY);
    if (fd_in < 0) {
        Logger::error(std::format("Failed to open file for reading: {} - {}", input.string(), strerror(errno)));
        return false;
    }
    
    // Подсказка ОС о последовательном чтении
    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);
    
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