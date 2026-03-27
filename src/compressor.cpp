#include "compressor.h"
#include "logger.h"
#include <fstream>
#include <vector>
#include <zlib.h>
#include <brotli/encode.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <format>
#include <algorithm>
#include <cerrno>
#include <cstring>      // <--- ДОБАВЛЕНО: для strerror()

// Опциональная поддержка SELinux
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

bool Compressor::compress_gzip(const fs::path& input, const fs::path& output, int level) {
    std::ifstream ifs(input, std::ios::binary | std::ios::ate);
    if (!ifs) {
        Logger::error(std::format("Failed to open file for reading: {}", input.string()));
        return false;
    }

    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (!ifs.read(buffer.data(), size)) {
        Logger::error(std::format("Failed to read file: {}", input.string()));
        return false;
    }
    ifs.close();

    z_stream strm = {};
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        Logger::error("Failed to init gzip stream");
        return false;
    }

    std::ofstream ofs(output, std::ios::binary);
    if (!ofs) {
        deflateEnd(&strm);
        Logger::error(std::format("Failed to open output file: {}", output.string()));
        return false;
    }

    strm.avail_in = buffer.size();
    strm.next_in = (Bytef*)buffer.data();
    
    char out_buffer[8192];
    int ret;
    do {
        strm.avail_out = sizeof(out_buffer);
        strm.next_out = (Bytef*)out_buffer;
        ret = deflate(&strm, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            Logger::error("Gzip stream error");
            deflateEnd(&strm);
            return false;
        }
        size_t have = sizeof(out_buffer) - strm.avail_out;
        if (have > 0) {
            ofs.write(out_buffer, have);
        }
    } while (ret == Z_OK);

    deflateEnd(&strm);
    ofs.close();
    
    if (ofs.fail()) {
        Logger::error("Failed to write gzip output");
        return false;
    }

    Logger::debug(std::format("Gzip compressed: {} -> {}", input.string(), output.string()));
    return true;
}

bool Compressor::compress_brotli(const fs::path& input, const fs::path& output, int level) {
    std::ifstream ifs(input, std::ios::binary | std::ios::ate);
    if (!ifs) {
        Logger::error(std::format("Failed to open file for reading: {}", input.string()));
        return false;
    }

    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<uint8_t> input_buffer(size);
    if (!ifs.read(reinterpret_cast<char*>(input_buffer.data()), size)) {
        Logger::error(std::format("Failed to read file: {}", input.string()));
        return false;
    }
    ifs.close();

    size_t encoded_size = BrotliEncoderMaxCompressedSize(input_buffer.size());
    std::vector<uint8_t> output_buffer(encoded_size);

    size_t actual_encoded_size = encoded_size;
    if (!BrotliEncoderCompress(level, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
                               input_buffer.size(), input_buffer.data(),
                               &actual_encoded_size, output_buffer.data())) {
        Logger::error(std::format("Brotli compression failed for: {}", input.string()));
        return false;
    }

    std::ofstream ofs(output, std::ios::binary);
    if (!ofs) {
        Logger::error(std::format("Failed to open output file: {}", output.string()));
        return false;
    }
    
    ofs.write(reinterpret_cast<char*>(output_buffer.data()), actual_encoded_size);
    ofs.close();

    if (ofs.fail()) {
        Logger::error("Failed to write brotli output");
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