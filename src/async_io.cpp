#include "async_io.h"
#include "logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <atomic>
#include <array>
#include <mutex>
#include <chrono>
#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif

// Проверяем доступность io_uring через liburing
#if __has_include(<liburing.h>)
#define HAS_IO_URING 1
#include <liburing.h>
#else
#define HAS_IO_URING 0
#endif

namespace {
    std::atomic<bool> g_uring_available{false};
}

#if HAS_IO_URING
namespace {
    io_uring g_ring = {};
    std::mutex g_ring_mutex;  // Мьютекс для защиты доступа к g_ring
    std::atomic<size_t> g_pending_submissions{0};

    // Структура для хранения контекста асинхронной операции
    struct AsyncIOContext {
        int fd = -1;
        uint8_t* buffer = nullptr;
        size_t expected_size = 0;
        bool is_write = false;
        std::atomic<bool>* completed = nullptr;
    };
}
#endif

AsyncIO::UringState AsyncIO::uring_state_{};

bool AsyncIO::is_uring_available() {
#if HAS_IO_URING
    // Проверяем возможность создания io_uring
    io_uring test_ring;
    int ret = io_uring_queue_init(32, &test_ring, 0);
    if (ret == 0) {
        io_uring_queue_exit(&test_ring);
        return true;
    }
    // io_uring не доступен (старое ядро или ограничения)
    return false;
#else
    return false;
#endif
}

bool AsyncIO::init_uring(size_t ring_size) {
#if HAS_IO_URING
    // Используем call_once для потокобезопасной инициализации
    static std::once_flag init_flag;
    static bool init_result = false;

    std::call_once(init_flag, [ring_size]() {
        std::lock_guard<std::mutex> lock(g_ring_mutex);
        if (uring_state_.initialized) {
            init_result = true;
            return;
        }

        // Инициализируем io_uring с флагом IORING_SETUP_SINGLE_ISSUER для лучшей производительности
        io_uring_params params = {};
        params.flags = IORING_SETUP_SINGLE_ISSUER;

        int ret = io_uring_queue_init_params(ring_size, &g_ring, &params);
        if (ret != 0) {
            // Пробуем без флагов для обратной совместимости
            ret = io_uring_queue_init(ring_size, &g_ring, 0);
            if (ret != 0) {
                Logger::warning(std::format("io_uring initialization failed: {}, falling back to sync I/O",
                                           strerror(-ret)));
                init_result = false;
                return;
            }
        }

        uring_state_.ring = &g_ring;
        uring_state_.initialized = true;
        uring_state_.ring_size = ring_size;
        g_uring_available.store(true);

        Logger::debug(std::format("io_uring initialized with ring size {} (single issuer: {})",
                                 ring_size, (params.flags & IORING_SETUP_SINGLE_ISSUER) ? "yes" : "no"));
        init_result = true;
    });

    return init_result;
#else
    Logger::debug("io_uring not available (liburing not installed), using sync I/O");
    return false;
#endif
}

void AsyncIO::cleanup_uring() {
#if HAS_IO_URING
    std::lock_guard<std::mutex> lock(g_ring_mutex);
    if (uring_state_.initialized) {
        io_uring_queue_exit(&g_ring);
        uring_state_.initialized = false;
        uring_state_.ring = nullptr;
        g_uring_available.store(false);
        Logger::debug("io_uring cleaned up");
    }
#endif
}

size_t AsyncIO::align_for_direct_io(size_t size) {
    return (size + DIRECT_IO_ALIGNMENT - 1) & ~(DIRECT_IO_ALIGNMENT - 1);
}

bool AsyncIO::is_aligned_for_direct_io(const void* ptr) {
    return (reinterpret_cast<uintptr_t>(ptr) % DIRECT_IO_ALIGNMENT) == 0;
}

ssize_t AsyncIO::sync_read(int fd, void* buffer, size_t size) {
    ssize_t total_read = 0;
    uint8_t* buf_ptr = static_cast<uint8_t*>(buffer);
    
    while (total_read < static_cast<ssize_t>(size)) {
        ssize_t bytes = read(fd, buf_ptr + total_read, size - total_read);
        if (bytes < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        if (bytes == 0) {
            break;  // EOF
        }
        total_read += bytes;
    }
    
    return total_read;
}

ssize_t AsyncIO::sync_write(int fd, const void* buffer, size_t size) {
    ssize_t total_written = 0;
    const uint8_t* buf_ptr = static_cast<const uint8_t*>(buffer);
    
    while (total_written < static_cast<ssize_t>(size)) {
        ssize_t bytes = write(fd, buf_ptr + total_written, size - total_written);
        if (bytes < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        if (bytes == 0) {
            return -1;  // Неожиданный случай
        }
        total_written += bytes;
    }
    
    return total_written;
}

int AsyncIO::open_file_optimized(const fs::path& path, int flags, mode_t mode) {
    // Добавляем оптимизационные флаги по умолчанию
    int optimized_flags = flags;

    // O_NOATIME требует CAP_FOWNER или совпадающего UID с владельцем файла.
    // После drop_privileges процесс может работать от другого пользователя,
    // поэтому O_NOATIME не добавляем — ядро может игнорировать его с EPERM.
    // Время доступа обновляется нечасто и не критично для работы сервиса.
    
    // Для записи добавляем O_SYNC для гарантированной записи на диск
    if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_CREAT)) {
        // O_SYNC может замедлить работу, используем только если важно
        // optimized_flags |= O_SYNC;
    }
    
    int fd = open(path.c_str(), optimized_flags, mode);
    if (fd < 0) {
        Logger::error(std::format("Failed to open file {}: {}", path.string(), strerror(errno)));
        return -1;
    }
    
    return fd;
}

bool AsyncIO::close_file_sync(int fd, bool sync_data) {
    if (fd < 0) {
        return false;
    }
    
    if (sync_data) {
        if (fsync(fd) != 0) {
            Logger::warning(std::format("fsync failed: {}", strerror(errno)));
            // Не считаем это фатальной ошибкой
        }
    }
    
    return close(fd) == 0;
}

bool AsyncIO::async_read_file(const fs::path& path, uint8_t* buffer,
                              size_t buffer_size, size_t offset,
                              size_t& bytes_read, bool use_direct_io) {
#if HAS_IO_URING
    if (!uring_state_.initialized) {
        // Fallback на синхронное чтение
        int flags = O_RDONLY;
        if (use_direct_io) {
            flags |= O_DIRECT;
        }
        int fd = open_file_optimized(path, flags);
        if (fd < 0) {
            return false;
        }
        
        // Используем posix_fadvise для оптимизации чтения
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        if (offset > 0) {
            posix_fadvise(fd, offset, buffer_size, POSIX_FADV_WILLNEED);
        }
        
        ssize_t result = sync_read(fd, buffer, buffer_size);
        close(fd);
        
        if (result < 0) {
            return false;
        }
        bytes_read = static_cast<size_t>(result);
        return true;
    }
    
    // Асинхронное чтение через io_uring с использованием linked SQE
    int flags = O_RDONLY;
    if (use_direct_io) {
        flags |= O_DIRECT;
    }
    
    int fd = open_file_optimized(path, flags);
    if (fd < 0) {
        return false;
    }
    
    // Получаем SQE для операции чтения
    io_uring_sqe* sqe = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_ring_mutex);
        sqe = io_uring_get_sqe(&g_ring);
        if (!sqe) {
            // Кольцо заполнено, выполняем submit и ждем
            io_uring_submit_and_wait(&g_ring, 1);
            sqe = io_uring_get_sqe(&g_ring);
            if (!sqe) {
                close(fd);
                Logger::error("io_uring SQE queue full");
                return false;
            }
        }

        // Подготовка операции чтения с поддержкой IOSQE_ASYNC
        io_uring_prep_read(sqe, fd, buffer, buffer_size, offset);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<intptr_t>(fd)));

        // Отправляем операцию
        int ret = io_uring_submit(&g_ring);
        if (ret < 0) {
            close(fd);
            Logger::error(std::format("io_uring submit failed: {}", strerror(-ret)));
            return false;
        }
    }
    
    // Ждем завершения операции с таймаутом 30 секунд
    io_uring_cqe* cqe = nullptr;
    struct __kernel_timespec timeout;
    timeout.tv_sec = 30;
    timeout.tv_nsec = 0;

    int ret;
    {
        std::lock_guard<std::mutex> lock(g_ring_mutex);
        ret = io_uring_wait_cqe_timeout(&g_ring, &cqe, &timeout);
    }
    if (ret < 0) {
        if (ret == -ETIME) {
            Logger::error("io_uring operation timed out after 30 seconds");
        } else {
            Logger::error(std::format("io_uring wait_cqe failed: {}", strerror(-ret)));
        }
        close(fd);
        return false;
    }

    if (cqe && cqe->res < 0) {
        Logger::error(std::format("async read failed: {}", strerror(-cqe->res)));
        {
            std::lock_guard<std::mutex> lock(g_ring_mutex);
            io_uring_cqe_seen(&g_ring, cqe);
        }
        close(fd);
        return false;
    }

    if (cqe) {
        bytes_read = static_cast<size_t>(cqe->res);
        {
            std::lock_guard<std::mutex> lock(g_ring_mutex);
            io_uring_cqe_seen(&g_ring, cqe);
        }
    } else {
        // Fallback на синхронное чтение если CQE не получен
        ssize_t result = sync_read(fd, buffer, buffer_size);
        if (result < 0) {
            close(fd);
            return false;
        }
        bytes_read = static_cast<size_t>(result);
    }

    // Закрываем fd только после завершения всех операций с ним
    close(fd);
    return true;
#else
    // Fallback без io_uring
    int flags = O_RDONLY;
    if (use_direct_io) {
        flags |= O_DIRECT;
    }
    
    int fd = open_file_optimized(path, flags);
    if (fd < 0) {
        return false;
    }
    
    // Используем posix_fadvise для оптимизации чтения
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    
    // Предзагрузка следующей порции данных
    if (offset > 0) {
        posix_fadvise(fd, offset, buffer_size, POSIX_FADV_WILLNEED);
    }
    
    ssize_t result = sync_read(fd, buffer, buffer_size);
    close(fd);
    
    if (result < 0) {
        return false;
    }
    bytes_read = static_cast<size_t>(result);
    return true;
#endif
}

bool AsyncIO::async_write_file(const fs::path& path, const uint8_t* buffer,
                               size_t size, size_t offset,
                               size_t& bytes_written, bool use_direct_io,
                               bool create_excl) {
#if HAS_IO_URING
    if (!uring_state_.initialized) {
        // Fallback на синхронную запись
        int flags = O_WRONLY | O_CREAT;
        if (create_excl) {
            flags |= O_EXCL;
        }
        if (use_direct_io) {
            flags |= O_DIRECT;
        }
        
        int fd = open_file_optimized(path, flags, 0644);
        if (fd < 0) {
            return false;
        }
        
        ssize_t result = sync_write(fd, buffer, size);
        close_file_sync(fd, true);
        
        if (result < 0) {
            return false;
        }
        bytes_written = static_cast<size_t>(result);
        return true;
    }
    
    // Асинхронная запись через io_uring
    int flags = O_WRONLY | O_CREAT;
    if (create_excl) {
        flags |= O_EXCL;
    }
    if (use_direct_io) {
        flags |= O_DIRECT;
    }
    
    int fd = open_file_optimized(path, flags, 0644);
    if (fd < 0) {
        return false;
    }
    
    io_uring_sqe* sqe = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_ring_mutex);
        sqe = io_uring_get_sqe(&g_ring);
        if (!sqe) {
            io_uring_submit(&g_ring);
            sqe = io_uring_get_sqe(&g_ring);
            if (!sqe) {
                close(fd);
                Logger::error("io_uring SQE queue full");
                return false;
            }
        }

        io_uring_prep_write(sqe, fd, buffer, size, offset);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<intptr_t>(fd)));

        int ret = io_uring_submit(&g_ring);
        if (ret < 0) {
            close(fd);
            Logger::error(std::format("io_uring submit failed: {}", strerror(-ret)));
            return false;
        }
    }
    
    // Ждем завершения операции с таймаутом 30 секунд
    io_uring_cqe* cqe = nullptr;
    struct __kernel_timespec timeout;
    timeout.tv_sec = 30;
    timeout.tv_nsec = 0;

    int ret;
    {
        std::lock_guard<std::mutex> lock(g_ring_mutex);
        ret = io_uring_wait_cqe_timeout(&g_ring, &cqe, &timeout);
    }
    if (ret < 0) {
        if (ret == -ETIME) {
            Logger::error("io_uring write operation timed out after 30 seconds");
        } else {
            Logger::error(std::format("io_uring wait_cqe failed: {}", strerror(-ret)));
        }
        close(fd);
        return false;
    }

    if (cqe && cqe->res < 0) {
        Logger::error(std::format("async write failed: {}", strerror(-cqe->res)));
        {
            std::lock_guard<std::mutex> lock(g_ring_mutex);
            io_uring_cqe_seen(&g_ring, cqe);
        }
        close_file_sync(fd, false);
        return false;
    }

    // Проверяем что все данные были записаны
    bytes_written = static_cast<size_t>(cqe->res);
    if (bytes_written != size) {
        Logger::warning(std::format("Partial write: {} of {} bytes written", bytes_written, size));
    }
    {
        std::lock_guard<std::mutex> lock(g_ring_mutex);
        io_uring_cqe_seen(&g_ring, cqe);
    }
    return close_file_sync(fd, !use_direct_io);
#else
    // Fallback без io_uring
    int flags = O_WRONLY | O_CREAT;
    if (create_excl) {
        flags |= O_EXCL;
    }
    if (use_direct_io) {
        flags |= O_DIRECT;
    }
    
    int fd = open_file_optimized(path, flags, 0644);
    if (fd < 0) {
        return false;
    }
    
    ssize_t result = sync_write(fd, buffer, size);
    close_file_sync(fd, true);
    
    if (result < 0) {
        return false;
    }
    bytes_written = static_cast<size_t>(result);
    return true;
#endif
}
