#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <optional>

namespace fs = std::filesystem;

/**
 * AsyncIO - класс для асинхронных I/O операций с использованием io_uring
 * 
 * Оптимизации:
 * - Использование io_uring для асинхронного чтения/записи без блокировок
 * - Прямые системные вызовы read/write вместо FILE* для снижения накладных расходов
 * - Поддержка O_DIRECT для прямого доступа к диску (минуя кэш страницы)
 * - Выровненные буферы для работы с O_DIRECT
 * - Предварительное выделение структур io_uring для снижения латентности
 */
class AsyncIO {
public:
    // Инициализация io_uring кольца
    static bool init_uring(size_t ring_size = 128);
    
    // Очистка ресурсов io_uring
    static void cleanup_uring();
    
    // Проверка доступности io_uring в системе
    static bool is_uring_available();
    
    /**
     * Асинхронное чтение файла с использованием io_uring
     * @param path Путь к файлу
     * @param buffer Буфер для чтения (должен быть выровнен для O_DIRECT)
     * @param offset Смещение в файле
     * @param bytes_read Количество прочитанных байт
     * @param use_direct_io Использовать O_DIRECT
     * @return true при успехе
     */
    static bool async_read_file(const fs::path& path, uint8_t* buffer, 
                                size_t buffer_size, size_t offset,
                                size_t& bytes_read, bool use_direct_io = false);
    
    /**
     * Асинхронная запись в файл с использованием io_uring
     * @param path Путь к файлу
     * @param buffer Буфер с данными
     * @param size Размер данных
     * @param offset Смещение в файле
     * @param bytes_written Количество записанных байт
     * @param use_direct_io Использовать O_DIRECT
     * @param create_excl Создать файл исключительно (O_CREAT | O_EXCL)
     * @return true при успехе
     */
    static bool async_write_file(const fs::path& path, const uint8_t* buffer,
                                 size_t size, size_t offset,
                                 size_t& bytes_written, bool use_direct_io = false,
                                 bool create_excl = false);
    
    /**
     * Синхронное чтение файла с использованием системных вызовов
     * Оптимизировано для последовательного чтения
     * @param fd Дескриптор файла
     * @param buffer Буфер для чтения
     * @param size Размер буфера
     * @return Количество прочитанных байт или -1 при ошибке
     */
    static ssize_t sync_read(int fd, void* buffer, size_t size);
    
    /**
     * Синхронная запись в файл с использованием системных вызовов
     * @param fd Дескриптор файла
     * @param buffer Буфер с данными
     * @param size Размер данных
     * @return Количество записанных байт или -1 при ошибке
     */
    static ssize_t sync_write(int fd, const void* buffer, size_t size);
    
    /**
     * Открытие файла с оптимизированными флагами
     * @param path Путь к файлу
     * @param flags Флаги открытия
     * @param mode Режим доступа
     * @return Дескриптор файла или -1 при ошибке
     */
    static int open_file_optimized(const fs::path& path, int flags, mode_t mode = 0644);
    
    /**
     * Закрытие файла с синхронизацией данных
     * @param fd Дескриптор файла
     * @param sync_data Выполнить fsync перед закрытием
     * @return true при успехе
     */
    static bool close_file_sync(int fd, bool sync_data = true);
    
    /**
     * Выравнивание размера по границе для O_DIRECT
     * @param size Исходный размер
     * @return Выровненный размер
     */
    static size_t align_for_direct_io(size_t size);
    
    /**
     * Проверка выравнивания адреса для O_DIRECT
     * @param ptr Указатель
     * @return true если выровнен
     */
    static bool is_aligned_for_direct_io(const void* ptr);
    
private:
    // Внутренняя структура для хранения состояния io_uring
    struct UringState {
        void* ring = nullptr;  // io_uring ring (используем void* для избежания include)
        bool initialized = false;
        size_t ring_size = 0;
    };
    
    static UringState uring_state_;
    
    // Выравнивание для O_DIRECT (должно быть кратно размеру сектора, обычно 512 байт)
    static constexpr size_t DIRECT_IO_ALIGNMENT = 4096;
};
