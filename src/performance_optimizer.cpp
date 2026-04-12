#include "performance_optimizer.h"
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <stdexcept>

#if __has_include(<format>)
#include <format>
#else
#include <fmt/format.h>
namespace std {
    using fmt::format;
}
#endif

#include "logger.h"
#include "i18n.h"

// Статические переменные
bool PerformanceOptimizer::huge_pages_initialized_ = false;
int PerformanceOptimizer::io_thread_count_ = -1;
int PerformanceOptimizer::cpu_thread_count_ = -1;
size_t PerformanceOptimizer::page_size_ = 0;
size_t PerformanceOptimizer::huge_page_size_ = 2 * 1024 * 1024; // 2MB по умолчанию

void PerformanceOptimizer::init(bool use_huge_pages, int io_threads, int cpu_threads) {
    // Получаем размер страницы
    page_size_ = sysconf(_SC_PAGESIZE);
    if (page_size_ <= 0) {
        page_size_ = 4096; // Значение по умолчанию
    }
    
    // Проверяем поддержку Huge Pages
    if (use_huge_pages && is_huge_pages_available()) {
        huge_pages_initialized_ = true;
        Logger::info(std::format(_("Huge Pages enabled ({} MB pages)", "Huge Pages включены (страницы {} МБ)"), huge_page_size_ / (1024 * 1024)));
    } else if (use_huge_pages) {
        Logger::warning(_("Huge Pages requested but not available, falling back to regular pages", "Запрошены Huge Pages, но недоступны, переход на обычные страницы"));
        huge_pages_initialized_ = false;
    }
    
    // Определяем количество потоков
    int cpu_count = get_cpu_count();
    
    if (io_threads > 0) {
        io_thread_count_ = io_threads;
    } else {
        // Для I/O операций оптимально: количество дисков или CPU ядер
        io_thread_count_ = cpu_count;
    }
    
    if (cpu_threads > 0) {
        cpu_thread_count_ = cpu_threads;
    } else {
        // Для CPU-bound операций (сжатие): меньше чем ядер для избежания contention
        cpu_thread_count_ = std::max(1, cpu_count - 1);
    }
    
    Logger::info(std::format(_("Performance optimizer initialized: {} I/O threads, {} CPU threads", "Оптимизатор производительности инициализирован: {} I/O потоков, {} CPU потоков"),
                             io_thread_count_, cpu_thread_count_));
}

bool PerformanceOptimizer::set_cpu_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    if (core_id < 0 || core_id >= get_cpu_count()) {
        Logger::warning(std::format(_("Invalid CPU core ID: {}", "Неверный ID ядра CPU: {}"), core_id));
        return false;
    }
    
    CPU_SET(core_id, &cpuset);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        Logger::error(std::format(_("Failed to set CPU affinity to core {}: {}", "Не удалось установить привязку CPU к ядру {}: {}"),
                                  core_id, strerror(errno)));
        return false;
    }
    
    Logger::debug(std::format(_("CPU affinity set to core {}", "Привязка CPU установлена к ядру {}"), core_id));
    return true;
}

bool PerformanceOptimizer::set_thread_cpu_affinity(std::thread& thread, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    if (core_id < 0 || core_id >= get_cpu_count()) {
        Logger::warning(std::format(_("Invalid CPU core ID for thread: {}", "Неверный ID ядра CPU для потока: {}"), core_id));
        return false;
    }
    
    CPU_SET(core_id, &cpuset);
    
    pthread_t native_handle = thread.native_handle();
    if (pthread_setaffinity_np(native_handle, sizeof(cpuset), &cpuset) != 0) {
        Logger::error(std::format(_("Failed to set thread CPU affinity to core {}: {}", "Не удалось установить привязку потока CPU к ядру {}: {}"),
                                  core_id, strerror(errno)));
        return false;
    }
    
    Logger::debug(std::format(_("Thread CPU affinity set to core {}", "Привязка потока CPU установлена к ядру {}"), core_id));
    return true;
}

PerformanceOptimizer::AllocatedMemory PerformanceOptimizer::allocate_aligned_memory(size_t size, bool use_huge_page) {
    void* ptr = nullptr;
    int method = 0;  // 0 = posix_memalign, 1 = mmap(Huge Pages), 2 = mmap(regular)

    // Выравниваем размер по границе страницы
    size_t aligned_size = (size + page_size_ - 1) & ~(page_size_ - 1);

    if (use_huge_page && huge_pages_initialized_) {
        // Попытка выделить память с Huge Pages через MAP_HUGETLB
        #ifdef MAP_HUGETLB
        ptr = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

        if (ptr == MAP_FAILED) {
            Logger::warning(std::format(_("Huge Pages allocation failed: {}, falling back to regular pages", "Выделение Huge Pages не удалось: {}, переход на обычные страницы"),
                                        strerror(errno)));
            ptr = nullptr;
        } else {
            Logger::debug(std::format(_("Allocated {} bytes with Huge Pages", "Выделено {} байт с Huge Pages"), aligned_size));
            method = 1;
            return {ptr, method, aligned_size};
        }
        #endif
    }

    // Fallback: выделение обычной памяти с выравниванием
    if (ptr == nullptr) {
        if (posix_memalign(&ptr, page_size_, aligned_size) != 0) {
            Logger::error(std::format(_("Failed to allocate aligned memory: {}", "Не удалось выделить выровненную память: {}"), strerror(errno)));
            return {nullptr, 0, 0};
        }
        Logger::debug(std::format(_("Allocated {} bytes with regular pages", "Выделено {} байт обычными страницами"), aligned_size));
        method = 0;
    }

    return {ptr, method, aligned_size};
}

void PerformanceOptimizer::free_aligned_memory(const AllocatedMemory& mem) {
    if (!mem.ptr) return;

    switch (mem.method) {
        case 1:  // mmap(Huge Pages)
        case 2:  // mmap(regular)
            munmap(mem.ptr, mem.size);
            break;
        case 0:  // posix_memalign
        default:
            free(mem.ptr);
            break;
    }
}

bool PerformanceOptimizer::preallocate_file(int fd, uint64_t size) {
    if (fd < 0) {
        Logger::error(_("Invalid file descriptor for preallocation", "Неверный файловый дескриптор для предварительного выделения"));
        return false;
    }
    
    // Используем fallocate для предварительного выделения места
    // Это предотвращает фрагментацию и улучшает производительность записи
    if (fallocate(fd, 0, 0, size) != 0) {
        // Fallback: ftruncate (менее эффективно, но работает на всех ФС)
        if (ftruncate(fd, size) != 0) {
            Logger::warning(std::format(_("Failed to preallocate file: {}", "Не удалось предварительно выделить файл: {}"), strerror(errno)));
            return false;
        }
    }
    
    Logger::debug(std::format(_("Preallocated {} bytes on FD {}", "Предварительно выделено {} байт на FD {}"), size, fd));
    return true;
}

bool PerformanceOptimizer::advise_file_access(int fd, bool sequential, bool no_reuse, bool write_once) {
    if (fd < 0) {
        Logger::error(_("Invalid file descriptor for advice", "Неверный файловый дескриптор для рекомендации"));
        return false;
    }
    
    int advice = 0;
    
    if (sequential) {
        advice |= POSIX_FADV_SEQUENTIAL;
    } else {
        advice |= POSIX_FADV_RANDOM;
    }
    
    if (no_reuse) {
        // POSIX_FADV_NOREUSE подсказывает ядру не кэшировать данные
        // (данные читаются один раз и не нужны повторно)
        advice |= POSIX_FADV_NOREUSE;
    }
    
    if (write_once) {
        // Для выходных файлов: запись один раз, потом только чтение
        advice |= POSIX_FADV_WILLNEED;
    }
    
    if (posix_fadvise(fd, 0, 0, advice) != 0) {
        // Это не ошибка — просто подсказка ОС, которую можно игнорировать
        Logger::debug(std::format(_("posix_fadvise failed (non-critical): {}", "posix_fadvise завершилась неудачно (не критично): {}"), strerror(errno)));
        return false;
    }
    
    Logger::debug(std::format(_("File access advice set: sequential={}, no_reuse={}, write_once={}", "Установлена рекомендация доступа: последовательно={}, не переиспользовать={}, однократная запись={}"),
                              sequential, no_reuse, write_once));
    return true;
}

int PerformanceOptimizer::get_cpu_count() {
    int count = sysconf(_SC_NPROCESSORS_ONLN);
    return (count > 0) ? count : 1;
}

bool PerformanceOptimizer::is_huge_pages_available() {
    // Проверяем наличие Huge Pages через /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        return false;
    }
    
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.find("HugePages_Total:") != std::string::npos) {
            // Извлекаем количество доступных Huge Pages
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string value_str = line.substr(colon_pos + 1);
                int huge_pages_count = std::stoi(value_str);
                return huge_pages_count > 0;
            }
        }
    }
    return false;
}
