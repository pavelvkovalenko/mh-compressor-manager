#pragma once
#include <cstdint>
#include <cstddef>

// x86-specific intrinsics
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define SIMD_UTILS_AVX2 1
#else
#define SIMD_UTILS_AVX2 0
#endif

/**
 * SIMDUtils - утилиты для SIMD-ускорения критических операций
 *
 * Оптимизации:
 * - AVX2/AVX-512 ускорение для копирования памяти (x86 only)
 * - SIMD-ускоренное обнуление буферов
 * - Векторизованное сложение и сравнение
 * - Prefetching данных для снижения латентности
 */
class SIMDUtils {
public:
    /**
     * Быстрое копирование памяти с использованием AVX2 (x86) или memcpy (другие)
     * @param dst Буфер назначения
     * @param src Исходный буфер
     * @param size Размер в байтах
     */
    static void fast_memcpy(void* dst, const void* src, size_t size) {
#if SIMD_UTILS_AVX2
        if (size < 32) {
            // Для малых размеров используем стандартное memcpy
            __builtin_memcpy(dst, src, size);
            return;
        }

        uint8_t* d = static_cast<uint8_t*>(dst);
        const uint8_t* s = static_cast<const uint8_t*>(src);

        // Копирование блоками по 32 байта (AVX2)
        size_t i = 0;
        for (; i + 32 <= size; i += 32) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + i),
                               _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i)));
        }

        // Остаток
        for (; i < size; ++i) {
            d[i] = s[i];
        }
#else
        // Fallback для не-x86 платформ
        __builtin_memcpy(dst, src, size);
#endif
    }

    /**
     * Быстрое обнуление памяти с использованием AVX2 (x86) или memset (другие)
     * @param ptr Буфер для обнуления
     * @param size Размер в байтах
     */
    static void fast_memzero(void* ptr, size_t size) {
#if SIMD_UTILS_AVX2
        if (size < 32) {
            __builtin_memset(ptr, 0, size);
            return;
        }

        uint8_t* p = static_cast<uint8_t*>(ptr);

        // Zero register для AVX2
        __m256i zero = _mm256_setzero_si256();

        // Обнуление блоками по 32 байта
        size_t i = 0;
        for (; i + 32 <= size; i += 32) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(p + i), zero);
        }

        // Остаток
        for (; i < size; ++i) {
            p[i] = 0;
        }
#else
        // Fallback для не-x86 платформ
        __builtin_memset(ptr, 0, size);
#endif
    }
    
    /**
     * Prefetch данных в кэш L1
     * @param ptr Указатель на данные
     * @param hint Тип prefetch (0-3)
     */
    static inline void prefetch(const void* ptr, int hint = _MM_HINT_T0) {
#if SIMD_UTILS_AVX2
        _mm_prefetch(static_cast<const char*>(ptr), hint);
#else
        // Fallback: __builtin_prefetch для не-x86
        __builtin_prefetch(ptr, 0, 3);  // read, high locality
#endif
    }

    /**
     * Prefetch следующей строки кэша для последовательного доступа
     * @param ptr Текущий указатель
     * @param stride Размер шага (обычно размер блока)
     */
    static inline void prefetch_next(const void* ptr, size_t stride = 64) {
#if SIMD_UTILS_AVX2
        _mm_prefetch(static_cast<const char*>(ptr) + stride, _MM_HINT_T0);
#else
        __builtin_prefetch(static_cast<const char*>(ptr) + stride, 0, 3);
#endif
    }
    
    /**
     * Проверка доступности AVX2
     * @return true если AVX2 доступен
     */
    static bool has_avx2() {
#if SIMD_UTILS_AVX2
#if defined(__AVX2__)
        return true;
#else
        // Runtime проверка через CPUID (x86 only)
        int cpu_info[4];
        __cpuid_count(7, 0, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
        return (cpu_info[1] & (1 << 5)) != 0;  // Bit 5 of EBX
#endif
#else
        return false;  // Не-x86: AVX2 не поддерживается
#endif
    }

private:
#if SIMD_UTILS_AVX2
    // Вспомогательная функция для CPUID (x86 only)
    static inline void __cpuid_count(int leaf, int subleaf, int& eax, int& ebx, int& ecx, int& edx) {
        __asm__ __volatile__(
            "cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(leaf), "c"(subleaf)
        );
    }
#endif
};
