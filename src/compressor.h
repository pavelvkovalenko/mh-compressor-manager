#pragma once
#include <string>
#include <vector>
#include <span>
#include <memory>
#include <optional>
#include <filesystem>
#include <cstdint>
#include <sys/stat.h>

// Forward-объявление для zlib
struct z_stream_s;
typedef z_stream_s z_stream;

// Brotli — нельзя forward-declare, нужен полный заголовок
#include <brotli/encode.h>

namespace fs = std::filesystem;

// Вспомогательная функция для валидации уровней сжатия
inline bool validate_compression_level(int level, int min_level, int max_level) {
    return level >= min_level && level <= max_level;
}

class Compressor {
public:
    static bool compress_gzip(const fs::path& input, const fs::path& output, int level);
    static bool compress_brotli(const fs::path& input, const fs::path& output, int level);

    /**
     * @brief Сжатие буфера в памяти форматом gzip
     * @param data Данные файла (не владеющий указатель, без копирования)
     * @param output_path Путь для записи .gz файла
     * @param level Уровень сжатия (1-9)
     * @return true при успехе
     */
    static bool compress_gzip_from_memory(std::span<const uint8_t> data,
                                           const fs::path& output_path, int level);

    /**
     * @brief Сжатие буфера в памяти форматом brotli
     * @param data Данные файла (не владеющий указатель, без копирования)
     * @param output_path Путь для записи .br файла
     * @param level Уровень сжатия (1-11)
     * @return true при успехе
     */
    static bool compress_brotli_from_memory(std::span<const uint8_t> data,
                                             const fs::path& output_path, int level);

    // ========================================================================
    // Streaming API для чанкового сжатия больших файлов (ТЗ §3.2.9, §21.3)
    // ========================================================================

    /**
     * @brief Удалитель для z_stream — вызывает deflateEnd при уничтожении
     */
    struct ZStreamDeleter {
        void operator()(z_stream* p) { if (p) { deflateEnd(p); delete p; } }
    };

    /**
     * @brief Удалитель для BrotliEncoderState
     */
    struct BrotliEncoderDeleter {
        void operator()(BrotliEncoderState* p) { if (p) BrotliEncoderDestroyInstance(p); }
    };

    /**
     * @brief Состояние streaming-сжатия gzip
     * Используется для чанковой обработки файлов > optimal_chunk_size
     */
    struct GzipStreamState {
        std::unique_ptr<z_stream, ZStreamDeleter> strm; ///< zlib stream (RAII)
        int fd_out;                              ///< Файловый дескриптор вывода
        std::string tmp_path;                    ///< Путь временного файла .gz.tmp
        std::string final_path;                  ///< Целевой путь .gz
        std::vector<uint8_t> out_buf;            ///< Буфер вывода (выделяется один раз, ТЗ §3.2.6)
        bool initialized;
        bool has_error;

        GzipStreamState() : strm(nullptr), fd_out(-1), initialized(false), has_error(false) {}
        // Деструктор не нужен — unique_ptr автоматически вызовет deflateEnd + delete
    };

    /**
     * @brief Состояние streaming-сжатия brotli
     */
    struct BrotliStreamState {
        std::unique_ptr<BrotliEncoderState, BrotliEncoderDeleter> enc; ///< Brotli encoder (RAII)
        int fd_out;                              ///< Файловый дескриптор вывода
        std::string tmp_path;                    ///< Путь временного файла .br.tmp
        std::string final_path;                  ///< Целевой путь .br
        std::vector<uint8_t> out_buf;            ///< Буфер вывода (выделяется один раз, ТЗ §3.2.6)
        bool initialized;
        bool has_error;
        bool finalized;                          ///< Защита от двойного flush

        BrotliStreamState() : enc(nullptr), fd_out(-1), initialized(false), has_error(false), finalized(false) {}
        // Деструктор не нужен — unique_ptr автоматически вызовет BrotliEncoderDestroyInstance
    };

    /**
     * @brief Начать streaming сжатие gzip
     * Открывает временный файл, инициализирует deflate
     */
    static bool gzip_stream_start(GzipStreamState& state, int level, const fs::path& output_path, mode_t src_mode);

    /**
     * @brief Передать чанк данных в streaming gzip
     * @param state Состояние потока
     * @param data Данные чанка
     * @param flush true для последнего чанка (Z_FINISH)
     */
    static bool gzip_stream_process(GzipStreamState& state, std::span<const uint8_t> data, bool flush);

    /**
     * @brief Начать streaming сжатие brotli
     */
    static bool brotli_stream_start(BrotliStreamState& state, int level, const fs::path& output_path, mode_t src_mode);

    /**
     * @brief Передать чанк данных в streaming brotli
     */
    static bool brotli_stream_process(BrotliStreamState& state, std::span<const uint8_t> data, bool flush);

private:
    // Backend-реализации для compress_*_from_memory
    static bool compress_gzip_zlib_from_memory(std::span<const uint8_t> data,
                                                const fs::path& output_path, int level);
#ifdef HAVE_LIBDEFLATE
    static bool compress_gzip_libdeflate_from_memory(std::span<const uint8_t> data,
                                                      const fs::path& output_path, int level);
#endif

public:

    static bool copy_metadata(const fs::path& source, const fs::path& dest);
    
    // Безопасное удаление сжатых копий с проверками
    static bool safe_remove_compressed(const fs::path& original_path);

private:
    // Внутренние проверки для safe_remove_compressed
    static bool check_file_ownership(const fs::path& path, uid_t expected_uid);
    static bool is_symlink_attack(const fs::path& path);
};