/**
 * Unit тесты для SIMDUtils
 * Проверяют корректность SIMD-оптимизаций
 */

#include <gtest/gtest.h>
#include "simd_utils.h"
#include <cstring>
#include <vector>
#include <random>

// Тест проверки доступности AVX2
TEST(SIMDUtilsTest, HasAVX2) {
    bool has_avx2 = SIMDUtils::has_avx2();
    
    SUCCEED() << "AVX2 available: " << (has_avx2 ? "yes" : "no");
}

// Тест быстрого копирования памяти (малый размер)
TEST(SIMDUtilsTest, FastMemcpySmall) {
    const size_t size = 16;  // Меньше 32 байт
    std::vector<uint8_t> src(size), dst(size);
    
    // Заполняем исходный буфер
    for (size_t i = 0; i < size; ++i) {
        src[i] = static_cast<uint8_t>(i);
    }
    
    // Копируем
    SIMDUtils::fast_memcpy(dst.data(), src.data(), size);
    
    // Проверяем
    EXPECT_EQ(src, dst);
}

// Тест быстрого копирования памяти (средний размер)
TEST(SIMDUtilsTest, FastMemcpyMedium) {
    const size_t size = 64;  // Кратно 32
    std::vector<uint8_t> src(size), dst(size);
    
    // Заполняем случайными данными
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    for (size_t i = 0; i < size; ++i) {
        src[i] = static_cast<uint8_t>(dis(gen));
    }
    
    SIMDUtils::fast_memcpy(dst.data(), src.data(), size);
    
    EXPECT_EQ(src, dst);
}

// Тест быстрого копирования памяти (большой размер)
TEST(SIMDUtilsTest, FastMemcpyLarge) {
    const size_t size = 4096;  // Большой буфер
    std::vector<uint8_t> src(size), dst(size);
    
    // Заполняем паттерном
    for (size_t i = 0; i < size; ++i) {
        src[i] = static_cast<uint8_t>(i % 256);
    }
    
    SIMDUtils::fast_memcpy(dst.data(), src.data(), size);
    
    EXPECT_EQ(src, dst);
}

// Тест быстрого копирования с нечетным размером
TEST(SIMDUtilsTest, FastMemcpyOddSize) {
    const size_t size = 100;  // Не кратно 32
    std::vector<uint8_t> src(size), dst(size);
    
    for (size_t i = 0; i < size; ++i) {
        src[i] = static_cast<uint8_t>(i);
    }
    
    SIMDUtils::fast_memcpy(dst.data(), src.data(), size);
    
    EXPECT_EQ(src, dst);
}

// Тест быстрого обнуления памяти (малый размер)
TEST(SIMDUtilsTest, FastMemzeroSmall) {
    const size_t size = 16;
    std::vector<uint8_t> buffer(size, 0xFF);
    
    SIMDUtils::fast_memzero(buffer.data(), size);
    
    for (size_t i = 0; i < size; ++i) {
        EXPECT_EQ(buffer[i], 0);
    }
}

// Тест быстрого обнуления памяти (средний размер)
TEST(SIMDUtilsTest, FastMemzeroMedium) {
    const size_t size = 128;
    std::vector<uint8_t> buffer(size, 0xAA);
    
    SIMDUtils::fast_memzero(buffer.data(), size);
    
    for (size_t i = 0; i < size; ++i) {
        EXPECT_EQ(buffer[i], 0);
    }
}

// Тест быстрого обнуления памяти (большой размер)
TEST(SIMDUtilsTest, FastMemzeroLarge) {
    const size_t size = 8192;
    std::vector<uint8_t> buffer(size, 0xBB);
    
    SIMDUtils::fast_memzero(buffer.data(), size);
    
    for (size_t i = 0; i < size; ++i) {
        EXPECT_EQ(buffer[i], 0);
    }
}

// Тест быстрого обнуления с нечетным размером
TEST(SIMDUtilsTest, FastMemzeroOddSize) {
    const size_t size = 50;
    std::vector<uint8_t> buffer(size, 0xCC);
    
    SIMDUtils::fast_memzero(buffer.data(), size);
    
    for (size_t i = 0; i < size; ++i) {
        EXPECT_EQ(buffer[i], 0);
    }
}

// Тест prefetch (просто проверяем что не падает)
TEST(SIMDUtilsTest, Prefetch) {
    std::vector<uint8_t> buffer(4096);
    
    // Prefetch с разными hint
    SIMDUtils::prefetch(buffer.data(), _MM_HINT_T0);
    SIMDUtils::prefetch(buffer.data(), _MM_HINT_T1);
    SIMDUtils::prefetch(buffer.data(), _MM_HINT_T2);
    SIMDUtils::prefetch(buffer.data(), _MM_HINT_NTA);
    
    SUCCEED() << "Prefetch operations completed without crash";
}

// Тест prefetch_next
TEST(SIMDUtilsTest, PrefetchNext) {
    std::vector<uint8_t> buffer(4096);
    
    // Prefetch следующей строки кэша
    SIMDUtils::prefetch_next(buffer.data(), 64);
    SIMDUtils::prefetch_next(buffer.data(), 128);
    SIMDUtils::prefetch_next(buffer.data());  // Default stride
    
    SUCCEED() << "Prefetch next operations completed without crash";
}

// Тест последовательного копирования и обнуления
TEST(SIMDUtilsTest, CopyThenZero) {
    const size_t size = 1024;
    std::vector<uint8_t> src(size), dst(size);
    
    // Заполняем src
    for (size_t i = 0; i < size; ++i) {
        src[i] = static_cast<uint8_t>(i % 256);
    }
    
    // Копируем
    SIMDUtils::fast_memcpy(dst.data(), src.data(), size);
    EXPECT_EQ(src, dst);
    
    // Обнуляем dst
    SIMDUtils::fast_memzero(dst.data(), size);
    
    for (size_t i = 0; i < size; ++i) {
        EXPECT_EQ(dst[i], 0);
    }
}

// Тест наложение буферов (overlap) - должно работать корректно
TEST(SIMDUtilsTest, OverlappingBuffers) {
    const size_t size = 128;
    std::vector<uint8_t> buffer(size);
    
    // Заполняем
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = static_cast<uint8_t>(i);
    }
    
    // Копирование с перекрытием (src начинается раньше dst)
    SIMDUtils::fast_memcpy(buffer.data() + 32, buffer.data(), 64);
    
    // Первые 32 байта должны остаться unchanged
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_EQ(buffer[i], static_cast<uint8_t>(i));
    }
    
    // Байты 32-95 должны быть скопированы из 0-63
    for (size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(buffer[32 + i], static_cast<uint8_t>(i));
    }
}

// Тест нулевого размера копирования
TEST(SIMDUtilsTest, ZeroSizeMemcpy) {
    std::vector<uint8_t> src(64), dst(64);
    
    // Заполняем dst чтобы проверить что не изменился
    std::fill(dst.begin(), dst.end(), 0xAB);
    
    SIMDUtils::fast_memcpy(dst.data(), src.data(), 0);
    
    // dst должен остаться unchanged
    for (size_t i = 0; i < dst.size(); ++i) {
        EXPECT_EQ(dst[i], 0xAB);
    }
}

// Тест нулевого размера обнуления
TEST(SIMDUtilsTest, ZeroSizeMemzero) {
    std::vector<uint8_t> buffer(64, 0xCD);
    
    SIMDUtils::fast_memzero(buffer.data(), 0);
    
    // Буфер должен остаться unchanged
    for (size_t i = 0; i < buffer.size(); ++i) {
        EXPECT_EQ(buffer[i], 0xCD);
    }
}

// Стресс-тест: множественные операции
TEST(SIMDUtilsTest, StressTest) {
    const size_t size = 2048;
    std::vector<uint8_t> src(size), dst(size);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    
    constexpr int iterations = 100;
    
    for (int i = 0; i < iterations; ++i) {
        // Заполняем случайными данными
        for (size_t j = 0; j < size; ++j) {
            src[j] = static_cast<uint8_t>(dis(gen));
        }
        
        // Копируем
        SIMDUtils::fast_memcpy(dst.data(), src.data(), size);
        EXPECT_EQ(src, dst);
        
        // Обнуляем
        SIMDUtils::fast_memzero(dst.data(), size);
        
        for (size_t j = 0; j < size; ++j) {
            EXPECT_EQ(dst[j], 0);
        }
    }
    
    SUCCEED() << "Stress test completed: " << iterations << " iterations";
}

// Тест производительности (информационный)
TEST(SIMDUtilsTest, PerformanceComparison) {
    const size_t size = 1024 * 1024;  // 1MB
    std::vector<uint8_t> src(size), dst(size);
    
    // Заполняем
    for (size_t i = 0; i < size; ++i) {
        src[i] = static_cast<uint8_t>(i % 256);
    }
    
    // SIMD memcpy
    auto start = std::chrono::high_resolution_clock::now();
    SIMDUtils::fast_memcpy(dst.data(), src.data(), size);
    auto end = std::chrono::high_resolution_clock::now();
    auto simd_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    // Стандартное memcpy для сравнения
    std::fill(dst.begin(), dst.end(), 0);
    start = std::chrono::high_resolution_clock::now();
    std::memcpy(dst.data(), src.data(), size);
    end = std::chrono::high_resolution_clock::now();
    auto std_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    
    SUCCEED() << "Performance (1MB copy): SIMD=" << simd_time << "us, std::memcpy=" << std_time << "us";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
