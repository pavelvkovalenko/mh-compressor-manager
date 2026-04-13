#pragma once
#include <cstdint>
#include <cstddef>
#include <sys/types.h>

/**
 * AsyncIO - утилиты для синхронных I/O операций
 *
 * Содержит только sync_read_full() — гарантированное чтение всего запрошенного
 * объёма данных с обработкой EINTR/EAGAIN.
 */
class AsyncIO {
public:
    /**
     * Синхронное чтение файла с гарантием чтения всех запрошенных байт
     * @param fd Дескриптор файла
     * @param buffer Буфер для чтения
     * @param size Размер буфера
     * @return Количество прочитанных байт или -1 при ошибке
     */
    static ssize_t sync_read_full(int fd, void* buffer, size_t size);
};
