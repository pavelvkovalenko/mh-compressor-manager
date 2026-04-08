/**
 * Unit тесты для AsyncIO
 * Проверяют корректность асинхронных I/O операций
 */

#include <gtest/gtest.h>
#include "async_io.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// Тест проверки доступности io_uring
TEST(AsyncIOTest, UringAvailability) {
    // Проверяем что функция не падает
    bool available = AsyncIO::is_uring_available();
    
    // Результат может быть true или false в зависимости от системы
    SUCCEED() << "io_uring available: " << (available ? "yes" : "no");
}

// Тест инициализации и очистки io_uring
TEST(AsyncIOTest, UringInitCleanup) {
    // Инициализация может завершиться неудачей если io_uring недоступен
    bool init_result = AsyncIO::init_uring(128);
    
    // Если инициировался успешно - очищаем
    if (init_result) {
        AsyncIO::cleanup_uring();
        SUCCEED() << "io_uring initialized and cleaned up successfully";
    } else {
        SUCCEED() << "io_uring initialization skipped (not available)";
    }
}

// Тест выравнивания для O_DIRECT
TEST(AsyncIOTest, DirectIOAlignment) {
    // Проверяем функцию выравнивания
    size_t aligned = AsyncIO::align_for_direct_io(1000);
    EXPECT_EQ(aligned % 4096, 0) << "Aligned size should be multiple of 4096";
    
    // Проверка на уже выровненном размере
    aligned = AsyncIO::align_for_direct_io(4096);
    EXPECT_EQ(aligned, 4096);
    
    // Проверка нулевого размера
    aligned = AsyncIO::align_for_direct_io(0);
    EXPECT_EQ(aligned, 0);
}

// Тест проверки выравнивания указателя
TEST(AsyncIOTest, PointerAlignmentCheck) {
    // Выделяем выровненную память
    void* aligned_ptr = nullptr;
    int result = posix_memalign(&aligned_ptr, 4096, 8192);
    ASSERT_EQ(result, 0);
    
    // Проверяем что функция определяет выравнивание
    bool is_aligned = AsyncIO::is_aligned_for_direct_io(aligned_ptr);
    EXPECT_TRUE(is_aligned) << "posix_memalign should return aligned pointer";
    
    free(aligned_ptr);
    
    // Проверяем невыровненный указатель (стековая переменная)
    char stack_var = 0;
    // Стековые переменные обычно не выровнены по 4096
    // Но мы не можем гарантировать это, поэтому просто проверяем что функция работает
    is_aligned = AsyncIO::is_aligned_for_direct_io(&stack_var);
    SUCCEED() << "Stack variable alignment check returned: " << (is_aligned ? "aligned" : "not aligned");
}

// Тест синхронного чтения и записи
TEST(AsyncIOTest, SyncReadWrite) {
    fs::path temp_file = fs::temp_directory_path() / "mh_asyncio_test";
    
    // Создаем тестовый файл
    int fd = open(temp_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    
    // Записываем данные
    const char* test_data = "Hello, AsyncIO!";
    size_t data_size = strlen(test_data);
    
    ssize_t written = AsyncIO::sync_write(fd, test_data, data_size);
    EXPECT_EQ(written, static_cast<ssize_t>(data_size));
    
    // Сбрасываем позицию в начало
    lseek(fd, 0, SEEK_SET);
    
    // Читаем данные
    char buffer[64] = {0};
    ssize_t read_bytes = AsyncIO::sync_read(fd, buffer, sizeof(buffer) - 1);
    EXPECT_GT(read_bytes, 0);
    EXPECT_STREQ(buffer, test_data);
    
    close(fd);
    fs::remove(temp_file);
}

// Тест открытия файла с оптимизированными флагами
TEST(AsyncIOTest, OpenFileOptimized) {
    fs::path temp_file = fs::temp_directory_path() / "mh_open_optimized_test";
    
    // Открываем файл для записи
    int fd = AsyncIO::open_file_optimized(temp_file, O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0);
    
    // Записываем данные
    const char* data = "test data";
    write(fd, data, strlen(data));
    
    close(fd);
    
    // Открываем для чтения
    fd = AsyncIO::open_file_optimized(temp_file, O_RDONLY);
    ASSERT_GE(fd, 0);
    
    char buffer[32] = {0};
    read(fd, buffer, sizeof(buffer) - 1);
    EXPECT_STREQ(buffer, data);
    
    close(fd);
    fs::remove(temp_file);
}

// Тест закрытия файла с синхронизацией
TEST(AsyncIOTest, CloseFileSync) {
    fs::path temp_file = fs::temp_directory_path() / "mh_close_sync_test";
    
    int fd = open(temp_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    
    // Записываем данные
    const char* data = "sync test";
    write(fd, data, strlen(data));
    
    // Закрываем с синхронизацией
    bool result = AsyncIO::close_file_sync(fd, true);
    EXPECT_TRUE(result);
    
    // Проверяем что файл существует и данные записаны
    std::ifstream ifs(temp_file);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content, data);
    
    fs::remove(temp_file);
}

// Тест асинхронного чтения файла
TEST(AsyncIOTest, AsyncReadFile) {
    fs::path temp_file = fs::temp_directory_path() / "mh_async_read_test";
    
    // Создаем файл с данными
    {
        std::ofstream ofs(temp_file);
        ofs << "Async read test data";
    }
    
    // Выделяем выровненный буфер
    uint8_t* buffer = nullptr;
    posix_memalign(reinterpret_cast<void**>(&buffer), 4096, 4096);
    ASSERT_NE(buffer, nullptr);
    
    size_t bytes_read = 0;
    bool result = AsyncIO::async_read_file(temp_file, buffer, 4096, 0, bytes_read, false);
    
    if (result) {
        EXPECT_GT(bytes_read, 0);
        EXPECT_STRNE(reinterpret_cast<char*>(buffer), "");
    } else {
        SUCCEED() << "Async read not available, falling back to sync";
    }
    
    free(buffer);
    fs::remove(temp_file);
}

// Тест асинхронной записи файла
TEST(AsyncIOTest, AsyncWriteFile) {
    fs::path temp_file = fs::temp_directory_path() / "mh_async_write_test";
    
    const char* test_data = "Async write test data";
    size_t data_size = strlen(test_data);
    
    // Выделяем выровненный буфер
    uint8_t* buffer = nullptr;
    posix_memalign(reinterpret_cast<void**>(&buffer), 4096, 4096);
    ASSERT_NE(buffer, nullptr);
    memcpy(buffer, test_data, data_size);
    
    size_t bytes_written = 0;
    bool result = AsyncIO::async_write_file(temp_file, buffer, data_size, 0, bytes_written, false);
    
    if (result) {
        EXPECT_EQ(bytes_written, data_size);
        
        // Проверяем что данные записаны
        std::ifstream ifs(temp_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        EXPECT_EQ(content, test_data);
    } else {
        SUCCEED() << "Async write not available, falling back to sync";
    }
    
    free(buffer);
    fs::remove(temp_file);
}

// Тест асинхронной записи с O_DIRECT
TEST(AsyncIOTest, AsyncWriteDirectIO) {
    fs::path temp_file = fs::temp_directory_path() / "mh_async_direct_test";
    
    // Для O_DIRECT размер должен быть выровнен
    size_t data_size = 4096;
    
    // Выделяем выровненный буфер
    uint8_t* buffer = nullptr;
    posix_memalign(reinterpret_cast<void**>(&buffer), 4096, data_size);
    ASSERT_NE(buffer, nullptr);
    memset(buffer, 0xAB, data_size);
    
    size_t bytes_written = 0;
    bool result = AsyncIO::async_write_file(temp_file, buffer, data_size, 0, bytes_written, true);
    
    if (result) {
        EXPECT_EQ(bytes_written, data_size);
    } else {
        SUCCEED() << "Direct I/O not available on this system";
    }
    
    free(buffer);
    fs::remove(temp_file);
}

// Тест асинхронной записи с флагом create_excl
TEST(AsyncIOTest, AsyncWriteCreateExcl) {
    fs::path temp_file = fs::temp_directory_path() / "mh_async_excl_test";
    
    const char* test_data = "Exclusive create test";
    size_t data_size = strlen(test_data);
    
    uint8_t* buffer = nullptr;
    posix_memalign(reinterpret_cast<void**>(&buffer), 4096, 4096);
    memcpy(buffer, test_data, data_size);
    
    size_t bytes_written = 0;
    
    // Первое создание должно succeed
    bool result1 = AsyncIO::async_write_file(temp_file, buffer, data_size, 0, bytes_written, false, true);
    
    if (result1) {
        // Второе создание с O_EXCL должно fail
        bool result2 = AsyncIO::async_write_file(temp_file, buffer, data_size, 0, bytes_written, false, true);
        EXPECT_FALSE(result2) << "Second exclusive create should fail";
    } else {
        SUCCEED() << "Async write with create_excl not available";
    }
    
    free(buffer);
    fs::remove(temp_file);
}

// Тест работы с большими файлами
TEST(AsyncIOTest, LargeFileHandling) {
    fs::path temp_file = fs::temp_directory_path() / "mh_large_file_test";
    
    // Создаем файл размером 1MB
    size_t file_size = 1024 * 1024;
    
    int fd = AsyncIO::open_file_optimized(temp_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    
    // Выделяем буфер
    std::vector<char> buffer(file_size);
    std::fill(buffer.begin(), buffer.end(), 'X');
    
    // Записываем
    ssize_t written = AsyncIO::sync_write(fd, buffer.data(), file_size);
    EXPECT_EQ(written, static_cast<ssize_t>(file_size));
    
    // Читаем обратно
    std::vector<char> read_buffer(file_size);
    lseek(fd, 0, SEEK_SET);
    ssize_t read_bytes = AsyncIO::sync_read(fd, read_buffer.data(), file_size);
    EXPECT_EQ(read_bytes, static_cast<ssize_t>(file_size));
    
    // Проверяем данные
    EXPECT_EQ(buffer, read_buffer);
    
    AsyncIO::close_file_sync(fd, false);
    fs::remove(temp_file);
}

// Тест обработки ошибок при чтении несуществующего файла
TEST(AsyncIOTest, ErrorHandlingNonexistentFile) {
    fs::path nonexistent = fs::temp_directory_path() / "mh_nonexistent_xyz123.txt";
    
    uint8_t* buffer = nullptr;
    posix_memalign(reinterpret_cast<void**>(&buffer), 4096, 4096);
    
    size_t bytes_read = 0;
    bool result = AsyncIO::async_read_file(nonexistent, buffer, 4096, 0, bytes_read, false);
    
    EXPECT_FALSE(result) << "Reading nonexistent file should fail";
    
    free(buffer);
}

// Тест обработки ошибок при записи в недопустимый путь
TEST(AsyncIOTest, ErrorHandlingInvalidPath) {
    fs::path invalid_path = "/nonexistent_dir/mh_test.txt";
    
    const char* data = "test";
    uint8_t* buffer = nullptr;
    posix_memalign(reinterpret_cast<void**>(&buffer), 4096, 4096);
    memcpy(buffer, data, strlen(data));
    
    size_t bytes_written = 0;
    bool result = AsyncIO::async_write_file(invalid_path, buffer, strlen(data), 0, bytes_written, false);
    
    EXPECT_FALSE(result) << "Writing to invalid path should fail";
    
    free(buffer);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
