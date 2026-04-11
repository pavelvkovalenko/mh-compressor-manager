#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>

// Forward-объявления для streaming структур
struct z_stream_s;
typedef z_stream_s z_stream;
struct BrotliEncoderStateStruct;
typedef struct BrotliEncoderStateStruct BrotliEncoderState;

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
     * @param data Указатель на данные файла (const, без копирования)
     * @param size Размер данных в байтах
     * @param output_path Путь для записи .gz файла
     * @param level Уровень сжатия (1-9)
     * @return true при успехе
     */
    static bool compress_gzip_from_memory(const uint8_t* data, size_t size,
                                           const fs::path& output_path, int level);

    /**
     * @brief Сжатие буфера в памяти форматом brotli
     * @param data Указатель на данные файла (const, без копирования)
     * @param size Размер данных в байтах
     * @param output_path Путь для записи .br файла
     * @param level Уровень сжатия (1-11)
     * @return true при успехе
     */
    static bool compress_brotli_from_memory(const uint8_t* data, size_t size,
                                             const fs::path& output_path, int level);

    // ========================================================================
    // Streaming API для чанкового сжатия больших файлов (ТЗ §3.2.9, §21.3)
    // ========================================================================

    /**
     * @brief Состояние streaming-сжатия gzip
     * Используется для чанковой обработки файлов > optimal_chunk_size
     */
    struct GzipStreamState {
        z_stream* strm;            ///< zlib stream (динамическое выделение)
        int fd_out;                ///< Файловый дескриптор вывода
        std::string tmp_path;      ///< Путь временного файла .gz.tmp
        std::string final_path;    ///< Целевой путь .gz
        bool initialized;
        bool has_error;

        GzipStreamState() : strm(nullptr), fd_out(-1), initialized(false), has_error(false) {}
        ~GzipStreamState();
    };

    /**
     * @brief Состояние streaming-сжатия brotli
     */
    struct BrotliStreamState {
        BrotliEncoderState* enc;   ///< Brotli encoder
        int fd_out;                ///< Файловый дескриптор вывода
        std::string tmp_path;      ///< Путь временного файла .br.tmp
        std::string final_path;    ///< Целевой путь .br
        bool initialized;
        bool has_error;

        BrotliStreamState() : enc(nullptr), fd_out(-1), initialized(false), has_error(false) {}
        ~BrotliStreamState();
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
     * @param size Размер чанка
     * @param flush true для последнего чанка (Z_FINISH)
     */
    static bool gzip_stream_process(GzipStreamState& state, const uint8_t* data, size_t size, bool flush);

    /**
     * @brief Начать streaming сжатие brotli
     */
    static bool brotli_stream_start(BrotliStreamState& state, int level, const fs::path& output_path, mode_t src_mode);

    /**
     * @brief Передать чанк данных в streaming brotli
     */
    static bool brotli_stream_process(BrotliStreamState& state, const uint8_t* data, size_t size, bool flush);

private:
    // Backend-реализации для compress_gzip_from_memory
    static bool compress_gzip_zlib_from_memory(const uint8_t* data, size_t size,
                                                const fs::path& output_path, int level);
#ifdef HAVE_LIBDEFLATE
    static bool compress_gzip_libdeflate_from_memory(const uint8_t* data, size_t size,
                                                      const fs::path& output_path, int level);
#endif

public:

    // Параллельное сжатие в оба формата за один проход чтения
    // ДЕПРЕЦИРОВАНО: функция содержит критическую гонку данных (data race)
    // на encoder state между основным и worker потоками.
    // Не используется в main.cpp. Для удаления в будущей версии.
    [[deprecated("compress_dual has a critical data race on encoder state and is not used. Use separate compress_gzip/compress_brotli calls instead.")]]
    static bool compress_dual(const fs::path& input,
                             const fs::path& gzip_output,
                             const fs::path& brotli_output,
                             int gzip_level,
                             int brotli_level);
    
    static bool copy_metadata(const fs::path& source, const fs::path& dest);
    
    // Безопасное удаление сжатых копий с проверками
    static bool safe_remove_compressed(const fs::path& original_path);
    static bool validate_path_in_directory(const fs::path& path, const std::vector<std::string>& allowed_dirs);
    static bool check_file_ownership(const fs::path& path, uid_t expected_uid);
    static bool is_symlink_attack(const fs::path& path);
};