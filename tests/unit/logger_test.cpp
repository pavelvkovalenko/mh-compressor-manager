/**
 * Unit тесты для Logger
 * Проверяют логирование и обработку сообщений
 */

#include <gtest/gtest.h>
#include "logger.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Настраиваем тестовое окружение
    }
    
    void TearDown() override {
        // Очищаем
    }
};

// Тест базового логирования info
TEST_F(LoggerTest, InfoLogging) {
    EXPECT_NO_THROW(Logger::info("Test info message"));
    SUCCEED() << "Info logging works";
}

// Тест логирования warning
TEST_F(LoggerTest, WarningLogging) {
    EXPECT_NO_THROW(Logger::warning("Test warning message"));
    SUCCEED() << "Warning logging works";
}

// Тест логирования error
TEST_F(LoggerTest, ErrorLogging) {
    EXPECT_NO_THROW(Logger::error("Test error message"));
    SUCCEED() << "Error logging works";
}

// Тест логирования debug
TEST_F(LoggerTest, DebugLogging) {
    EXPECT_NO_THROW(Logger::debug("Test debug message"));
    SUCCEED() << "Debug logging works";
}

// Тест логирования с пустым сообщением
TEST_F(LoggerTest, EmptyMessageLogging) {
    EXPECT_NO_THROW(Logger::info(""));
    EXPECT_NO_THROW(Logger::warning(""));
    EXPECT_NO_THROW(Logger::error(""));
    SUCCEED() << "Empty message logging works";
}

// Тест логирования с специальными символами
TEST_F(LoggerTest, SpecialCharactersLogging) {
    EXPECT_NO_THROW(Logger::info("Message with\ttab\nnewline"));
    EXPECT_NO_THROW(Logger::warning("Message with special chars: !@#$%^&*()"));
    SUCCEED() << "Special characters logging works";
}

// Тест логирования с unicode
TEST_F(LoggerTest, UnicodeLogging) {
    EXPECT_NO_THROW(Logger::info("Unicode test: Привет мир 你好世界"));
    SUCCEED() << "Unicode logging works";
}

// Тест на производительность логирования
TEST_F(LoggerTest, PerformanceLogging) {
    constexpr int iterations = 1000;
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        Logger::info("Performance test message " + std::to_string(i));
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    SUCCEED() << "Logged " << iterations << " messages in " << duration << "ms";
}

// Тест многопоточного логирования
TEST_F(LoggerTest, MultithreadedLogging) {
    std::vector<std::thread> threads;
    constexpr int num_threads = 10;
    constexpr int messages_per_thread = 100;
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < messages_per_thread; ++i) {
                Logger::info("Thread " + std::to_string(t) + " message " + std::to_string(i));
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    SUCCEED() << "Multithreaded logging completed successfully";
}

// Тест очень длинных сообщений
TEST_F(LoggerTest, LongMessageLogging) {
    std::string long_message(10000, 'x');
    EXPECT_NO_THROW(Logger::info(long_message));
    SUCCEED() << "Long message logging works";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
