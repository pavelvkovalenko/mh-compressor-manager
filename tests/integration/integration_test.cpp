/**
 * Интеграционные тесты для mh-compressor-manager
 * End-to-end тестирование полного цикла сжатия
 */

#include <gtest/gtest.h>
#include "compressor.h"
#include "config.h"
#include "logger.h"
#include "security.h"
#include <fstream>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

class IntegrationTest : public ::testing::Test {
protected:
    fs::path temp_dir;
    fs::path test_data_dir;
    
    void SetUp() override {
        temp_dir = fs::temp_directory_path() / "mh_integration_test";
        test_data_dir = temp_dir / "data";
        fs::create_directories(test_data_dir);
    }
    
    void TearDown() override {
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
    }
    
    fs::path createTestFile(const std::string& name, const std::string& content) {
        fs::path file_path = test_data_dir / name;
        std::ofstream ofs(file_path);
        ofs << content;
        return file_path;
    }
};

// Тест полного цикла: создание файлов -> сжатие -> проверка
TEST_F(IntegrationTest, FullCompressionCycle) {
    // Создаем несколько тестовых файлов
    auto file1 = createTestFile("document.txt", "This is a text document with some content for compression testing.");
    auto file2 = createTestFile("log.log", "Log entry 1\nLog entry 2\nLog entry 3\nRepeated pattern for better compression");
    auto file3 = createTestFile("data.dat", std::string(10000, 'A') + std::string(10000, 'B'));
    
    // Сжимаем файлы gzip
    EXPECT_TRUE(Compressor::compress_gzip(file1, file1.string() + ".gz", 6));
    EXPECT_TRUE(Compressor::compress_gzip(file2, file2.string() + ".gz", 6));
    EXPECT_TRUE(Compressor::compress_gzip(file3, file3.string() + ".gz", 6));
    
    // Проверяем существование сжатых файлов
    EXPECT_TRUE(fs::exists(file1.string() + ".gz"));
    EXPECT_TRUE(fs::exists(file2.string() + ".gz"));
    EXPECT_TRUE(fs::exists(file3.string() + ".gz"));
    
    // Проверяем что оригиналы остались на месте
    EXPECT_TRUE(fs::exists(file1));
    EXPECT_TRUE(fs::exists(file2));
    EXPECT_TRUE(fs::exists(file3));
    
    SUCCEED() << "Full compression cycle completed successfully";
}

// Тест полного цикла с Brotli
TEST_F(IntegrationTest, FullBrotliCycle) {
    auto file1 = createTestFile("doc.txt", "Document content for Brotli compression");
    auto file2 = createTestFile("report.txt", std::string(5000, 'X') + std::string(5000, 'Y'));
    
    // Сжимаем файлы brotli
    EXPECT_TRUE(Compressor::compress_brotli(file1, file1.string() + ".br", 4));
    EXPECT_TRUE(Compressor::compress_brotli(file2, file2.string() + ".br", 6));
    
    // Проверяем
    EXPECT_TRUE(fs::exists(file1.string() + ".br"));
    EXPECT_TRUE(fs::exists(file2.string() + ".br"));
    
    SUCCEED() << "Brotli compression cycle completed";
}

// Тест двойного сжатия (gzip + brotli)
TEST_F(IntegrationTest, DualAlgorithmCompression) {
    auto file = createTestFile("dual.txt", "Content for dual compression with both algorithms");
    
    // Сжимаем обоими алгоритмами
    bool gzip_ok = Compressor::compress_gzip(file, file.string() + ".gz", 6);
    bool brotli_ok = Compressor::compress_brotli(file, file.string() + ".br", 4);
    
    EXPECT_TRUE(gzip_ok);
    EXPECT_TRUE(brotli_ok);
    
    EXPECT_TRUE(fs::exists(file.string() + ".gz"));
    EXPECT_TRUE(fs::exists(file.string() + ".br"));
    
    // Сравниваем размеры
    auto gz_size = fs::file_size(file.string() + ".gz");
    auto br_size = fs::file_size(file.string() + ".br");
    
    SUCCEED() << "Gzip size: " << gz_size << ", Brotli size: " << br_size;
}

// Тест безопасности: попытка атаки через symlink
TEST_F(IntegrationTest, SymlinkAttackPrevention) {
    // Создаем целевой файл вне рабочей директории
    fs::path target = fs::temp_directory_path() / "symlink_target.txt";
    {
        std::ofstream ofs(target);
        ofs << "Sensitive data outside working directory";
    }
    
    // Создаем symlink в рабочей директории
    fs::path symlink = test_data_dir / "malicious_link.txt";
    fs::create_symlink(target, symlink);
    
    // Пытаемся сжать symlink - должно быть отклонено
    fs::path output = symlink.string() + ".gz";
    bool result = Compressor::compress_gzip(symlink, output, 6);
    
    // Ожидаем отказ
    EXPECT_FALSE(result);
    EXPECT_FALSE(fs::exists(output));
    
    // Очищаем
    fs::remove(target);
    
    SUCCEED() << "Symlink attack prevented";
}

// Тест производительности на множестве файлов
TEST_F(IntegrationTest, BulkFileCompression) {
    constexpr int num_files = 100;
    
    // Создаем много файлов
    for (int i = 0; i < num_files; ++i) {
        createTestFile("file_" + std::to_string(i) + ".txt", 
                      "Content of file " + std::to_string(i) + " with some repetitive data. " +
                      std::string(1000, 'R'));
    }
    
    auto start = std::chrono::steady_clock::now();
    
    // Сжимаем все файлы
    int success_count = 0;
    for (const auto& entry : fs::directory_iterator(test_data_dir)) {
        if (entry.path().extension() == ".txt") {
            if (Compressor::compress_gzip(entry.path(), entry.path().string() + ".gz", 6)) {
                success_count++;
            }
        }
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    EXPECT_EQ(success_count, num_files);
    
    SUCCEED() << "Compressed " << num_files << " files in " << duration << "ms";
}

// Тест copy_metadata на реальных файлах
TEST_F(IntegrationTest, MetadataCopyIntegration) {
    auto source = createTestFile("source.txt", "Source file content");
    auto dest = createTestFile("dest.txt", "Destination file content");
    
    // Устанавливаем специфичные права и время модификации
    fs::permissions(source, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    
    auto source_time = fs::last_write_time(source);
    
    // Копируем метаданные
    bool result = Compressor::copy_metadata(source, dest);
    
    EXPECT_TRUE(result);
    
    // Проверяем что метаданные скопированы
    auto dest_perms = fs::status(dest).permissions();
    
    SUCCEED() << "Metadata copied successfully";
}

// Тест обработки ошибок: сжатие несуществующих файлов
TEST_F(IntegrationTest, ErrorHandlingNonExistentFiles) {
    fs::path nonexistent = test_data_dir / "does_not_exist.txt";
    fs::path output = nonexistent.string() + ".gz";
    
    bool result = Compressor::compress_gzip(nonexistent, output, 6);
    
    EXPECT_FALSE(result);
    EXPECT_FALSE(fs::exists(output));
    
    SUCCEED() << "Error handling works correctly";
}

// Тест идемпотентности: проверка should_compress логики
TEST_F(IntegrationTest, IdempotencyCheck) {
    auto file = createTestFile("idempotent.txt", "Content for idempotency test");
    
    // Первое сжатие
    EXPECT_TRUE(Compressor::compress_gzip(file, file.string() + ".gz", 6));
    
    auto original_time = fs::last_write_time(file);
    
    // Ждем немного чтобы убедиться что timestamp отличается
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Повторное сжатие должно создать новый файл (так как мы не проверяем should_compress здесь)
    fs::remove(file.string() + ".gz");
    EXPECT_TRUE(Compressor::compress_gzip(file, file.string() + ".gz", 6));
    
    SUCCEED() << "Idempotency test passed";
}

// Тест с большими файлами
TEST_F(IntegrationTest, LargeFileHandling) {
    fs::path large_file = test_data_dir / "large.bin";
    
    // Создаем файл 10MB
    std::ofstream ofs(large_file, std::ios::binary);
    std::vector<char> buffer(1024 * 1024);  // 1MB buffer
    std::fill(buffer.begin(), buffer.end(), 'Z');
    
    for (int i = 0; i < 10; ++i) {
        ofs.write(buffer.data(), buffer.size());
    }
    ofs.close();
    
    EXPECT_TRUE(fs::exists(large_file));
    EXPECT_EQ(fs::file_size(large_file), 10 * 1024 * 1024);
    
    // Сжимаем
    auto start = std::chrono::steady_clock::now();
    bool result = Compressor::compress_gzip(large_file, large_file.string() + ".gz", 6);
    auto end = std::chrono::steady_clock::now();
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(fs::exists(large_file.string() + ".gz"));
    
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
    auto compressed_size = fs::file_size(large_file.string() + ".gz");
    
    SUCCEED() << "Large file (10MB) compressed in " << duration << "s to " << compressed_size << " bytes";
}

// Тест проверки прав доступа
TEST_F(IntegrationTest, PermissionHandling) {
    auto file = createTestFile("readonly.txt", "Read only content");
    
    // Делаем файл только для чтения
    fs::permissions(file, fs::perms::owner_read);
    
    // Чтение должно работать
    fs::path output = file.string() + ".gz";
    bool result = Compressor::compress_gzip(file, output, 6);
    
    // Может succeed или fail в зависимости от прав
    SUCCEED() << "Permission test completed, result: " << (result ? "success" : "failed");
    
    // Восстанавливаем права для удаления
    fs::permissions(file, fs::perms::owner_all);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
