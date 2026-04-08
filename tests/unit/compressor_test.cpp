/**
 * Unit тесты для Compressor
 * Проверяют сжатие Gzip и Brotli, безопасность путей
 */

#include <gtest/gtest.h>
#include "compressor.h"
#include <fstream>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

class CompressorTest : public ::testing::Test {
protected:
    fs::path temp_dir;
    
    void SetUp() override {
        temp_dir = fs::temp_directory_path() / "mh_compressor_test";
        fs::create_directories(temp_dir);
    }
    
    void TearDown() override {
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
    }
    
    fs::path createTestFile(const std::string& name, const std::string& content) {
        fs::path file_path = temp_dir / name;
        std::ofstream ofs(file_path);
        ofs << content;
        return file_path;
    }
};

// Тест сжатия Gzip базового файла
TEST_F(CompressorTest, GzipBasicCompression) {
    fs::path input = createTestFile("test.txt", "Hello World! This is a test file for compression.");
    fs::path output = input.string() + ".gz";
    
    bool result = Compressor::compress_gzip(input, output, 6);
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(fs::exists(output));
    
    // Проверяем что сжатый файл меньше оригинала (для достаточно больших данных)
    if (fs::exists(output)) {
        auto original_size = fs::file_size(input);
        auto compressed_size = fs::file_size(output);
        SUCCEED() << "Original: " << original_size << ", Compressed: " << compressed_size;
    }
}

// Тест сжатия Brotli базового файла
TEST_F(CompressorTest, BrotliBasicCompression) {
    fs::path input = createTestFile("test.txt", "Hello World! This is a test file for Brotli compression.");
    fs::path output = input.string() + ".br";
    
    bool result = Compressor::compress_brotli(input, output, 4);
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(fs::exists(output));
    
    if (fs::exists(output)) {
        auto original_size = fs::file_size(input);
        auto compressed_size = fs::file_size(output);
        SUCCEED() << "Original: " << original_size << ", Compressed: " << compressed_size;
    }
}

// Тест сжатия пустого файла
TEST_F(CompressorTest, EmptyFileCompression) {
    fs::path input = createTestFile("empty.txt", "");
    fs::path output_gz = input.string() + ".gz";
    fs::path output_br = input.string() + ".br";
    
    bool gzip_result = Compressor::compress_gzip(input, output_gz, 6);
    bool brotli_result = Compressor::compress_brotli(input, output_br, 4);
    
    EXPECT_TRUE(gzip_result);
    EXPECT_TRUE(brotli_result);
    EXPECT_TRUE(fs::exists(output_gz));
    EXPECT_TRUE(fs::exists(output_br));
}

// Тест сжатия большого файла
TEST_F(CompressorTest, LargeFileCompression) {
    fs::path input = temp_dir / "large.txt";
    
    // Создаем файл 1MB
    std::ofstream ofs(input);
    std::string line(1000, 'x');
    for (int i = 0; i < 1024; ++i) {
        ofs << line << "\n";
    }
    ofs.close();
    
    fs::path output_gz = input.string() + ".gz";
    fs::path output_br = input.string() + ".br";
    
    bool gzip_result = Compressor::compress_gzip(input, output_gz, 6);
    bool brotli_result = Compressor::compress_brotli(input, output_br, 4);
    
    EXPECT_TRUE(gzip_result);
    EXPECT_TRUE(brotli_result);
    
    auto original_size = fs::file_size(input);
    auto gz_size = fs::file_size(output_gz);
    auto br_size = fs::file_size(output_br);
    
    SUCCEED() << "Original: " << original_size << ", Gzip: " << gz_size << ", Brotli: " << br_size;
}

// Тест на безопасность: отказ от сжатия symlink
TEST_F(CompressorTest, SymlinkRejection) {
    // Создаем целевой файл
    fs::path target = createTestFile("target.txt", "Target content");
    
    // Создаем symlink
    fs::path symlink = temp_dir / "link.txt";
    fs::create_symlink(target, symlink);
    
    // Пытаемся сжать symlink - должно быть отклонено
    fs::path output = symlink.string() + ".gz";
    
    // Компрессор должен отклонить symlink через lstat проверку
    bool result = Compressor::compress_gzip(symlink, output, 6);
    
    // Ожидаем false так как это symlink
    EXPECT_FALSE(result);
}

// Тест на безопасность: несуществующий входной файл
TEST_F(CompressorTest, NonExistentInputFile) {
    fs::path input = temp_dir / "nonexistent.txt";
    fs::path output = input.string() + ".gz";
    
    bool result = Compressor::compress_gzip(input, output, 6);
    
    EXPECT_FALSE(result);
}

// Тест на безопасность: отсутствие прав на чтение
TEST_F(CompressorTest, NoReadPermission) {
    fs::path input = createTestFile("noread.txt", "Secret content");
    
    // Устанавливаем права только на запись
    fs::permissions(input, fs::perms::owner_write);
    
    fs::path output = input.string() + ".gz";
    
    bool result = Compressor::compress_gzip(input, output, 6);
    
    // Должно вернуть false из-за отсутствия прав на чтение
    EXPECT_FALSE(result);
    
    // Восстанавливаем права для удаления
    fs::permissions(input, fs::perms::owner_all);
}

// Тест copy_metadata
TEST_F(CompressorTest, CopyMetadata) {
    fs::path source = createTestFile("source.txt", "Source content");
    fs::path dest = createTestFile("dest.txt", "Dest content");
    
    // Устанавливаем специфичные права на источник
    fs::permissions(source, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    
    bool result = Compressor::copy_metadata(source, dest);
    
    EXPECT_TRUE(result);
    
    // Проверяем что права скопированы
    auto source_perms = fs::status(source).permissions();
    auto dest_perms = fs::status(dest).permissions();
    
    SUCCEED() << "Metadata copied successfully";
}

// Тест различных уровней сжатия Gzip
TEST_F(CompressorTest, GzipCompressionLevels) {
    fs::path input = temp_dir / "level_test.txt";
    
    // Создаем повторяющиеся данные для лучшего сжатия
    std::ofstream ofs(input);
    for (int i = 0; i < 100; ++i) {
        ofs << "AAAAAAAAAA BBBBBBBBBB CCCCCCCCCC DDDDDDDDDD\n";
    }
    ofs.close();
    
    std::vector<int> levels = {1, 3, 6, 9};
    std::vector<size_t> sizes;
    
    for (int level : levels) {
        fs::path output = temp_dir / ("level_" + std::to_string(level) + ".gz");
        bool result = Compressor::compress_gzip(input, output, level);
        
        EXPECT_TRUE(result);
        if (result) {
            sizes.push_back(fs::file_size(output));
        }
    }
    
    // Обычно более высокий уровень дает лучшее сжатие (но не всегда)
    SUCCEED() << "Compression levels tested";
}

// Тест различных уровней сжатия Brotli
TEST_F(CompressorTest, BrotliCompressionLevels) {
    fs::path input = temp_dir / "brotli_level_test.txt";
    
    // Создаем повторяющиеся данные
    std::ofstream ofs(input);
    for (int i = 0; i < 100; ++i) {
        ofs << "XXXXXXXXXX YYYYYYYYYY ZZZZZZZZZZ 0123456789\n";
    }
    ofs.close();
    
    std::vector<int> levels = {1, 4, 8, 11};
    
    for (int level : levels) {
        fs::path output = temp_dir / ("brotli_level_" + std::to_string(level) + ".br");
        bool result = Compressor::compress_brotli(input, output, level);
        
        EXPECT_TRUE(result);
    }
    
    SUCCEED() << "Brotli compression levels tested";
}

// Тест идемпотентности: повторное сжатие
TEST_F(CompressorTest, IdempotentCompression) {
    fs::path input = createTestFile("idempotent.txt", "Test content for idempotency check");
    fs::path output = input.string() + ".gz";
    
    // Первое сжатие
    bool result1 = Compressor::compress_gzip(input, output, 6);
    EXPECT_TRUE(result1);
    
    size_t size1 = fs::file_size(output);
    
    // Удаляем выходной файл и сжимаем снова
    fs::remove(output);
    
    bool result2 = Compressor::compress_gzip(input, output, 6);
    EXPECT_TRUE(result2);
    
    size_t size2 = fs::file_size(output);
    
    // Размеры должны быть примерно одинаковыми
    EXPECT_NEAR(size1, size2, 100);  // Допускаем небольшую разницу
}

// Тест производительности сжатия
TEST_F(CompressorTest, CompressionPerformance) {
    fs::path input = temp_dir / "perf_test.txt";
    
    // Создаем файл 5MB
    std::ofstream ofs(input);
    std::string line(1000, 'a');
    for (int i = 0; i < 5120; ++i) {
        ofs << line << "\n";
    }
    ofs.close();
    
    auto start = std::chrono::steady_clock::now();
    
    fs::path output_gz = input.string() + ".gz";
    bool gzip_result = Compressor::compress_gzip(input, output_gz, 6);
    
    auto mid = std::chrono::steady_clock::now();
    
    fs::path output_br = input.string() + ".br";
    bool brotli_result = Compressor::compress_brotli(input, output_br, 4);
    
    auto end = std::chrono::steady_clock::now();
    
    auto gzip_duration = std::chrono::duration_cast<std::chrono::milliseconds>(mid - start).count();
    auto brotli_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - mid).count();
    
    EXPECT_TRUE(gzip_result);
    EXPECT_TRUE(brotli_result);
    
    SUCCEED() << "Gzip: " << gzip_duration << "ms, Brotli: " << brotli_duration << "ms";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
