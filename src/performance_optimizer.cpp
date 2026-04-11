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
        Logger::info(std::format("Huge Pages enabled ({} MB pages)", huge_page_size_ / (1024 * 1024)));
    } else if (use_huge_pages) {
        Logger::warning("Huge Pages requested but not available, falling back to regular pages");
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
    
    Logger::info(std::format("Performance optimizer initialized: {} I/O threads, {} CPU threads", 
                             io_thread_count_, cpu_thread_count_));
}

bool PerformanceOptimizer::set_cpu_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    if (core_id < 0 || core_id >= get_cpu_count()) {
        Logger::warning(std::format("Invalid CPU core ID: {}", core_id));
        return false;
    }
    
    CPU_SET(core_id, &cpuset);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        Logger::error(std::format("Failed to set CPU affinity to core {}: {}", 
                                  core_id, strerror(errno)));
        return false;
    }
    
    Logger::debug(std::format("CPU affinity set to core {}", core_id));
    return true;
}

bool PerformanceOptimizer::set_thread_cpu_affinity(std::thread& thread, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    if (core_id < 0 || core_id >= get_cpu_count()) {
        Logger::warning(std::format("Invalid CPU core ID for thread: {}", core_id));
        return false;
    }
    
    CPU_SET(core_id, &cpuset);
    
    pthread_t native_handle = thread.native_handle();
    if (pthread_setaffinity_np(native_handle, sizeof(cpuset), &cpuset) != 0) {
        Logger::error(std::format("Failed to set thread CPU affinity to core {}: {}", 
                                  core_id, strerror(errno)));
        return false;
    }
    
    Logger::debug(std::format("Thread CPU affinity set to core {}", core_id));
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
            Logger::warning(std::format("Huge Pages allocation failed: {}, falling back to regular pages",
                                        strerror(errno)));
            ptr = nullptr;
        } else {
            Logger::debug(std::format("Allocated {} bytes with Huge Pages", aligned_size));
            method = 1;
            return {ptr, method, aligned_size};
        }
        #endif
    }

    // Fallback: выделение обычной памяти с выравниванием
    if (ptr == nullptr) {
        if (posix_memalign(&ptr, page_size_, aligned_size) != 0) {
            Logger::error(std::format("Failed to allocate aligned memory: {}", strerror(errno)));
            return {nullptr, 0, 0};
        }
        Logger::debug(std::format("Allocated {} bytes with regular pages", aligned_size));
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

// Устаревший API — для обратной совместимости
void* PerformanceOptimizer::allocate_aligned_memory_old(size_t size, bool use_huge_page) {
    auto result = allocate_aligned_memory(size, use_huge_page);
    return result.ptr;
}

bool PerformanceOptimizer::preallocate_file(int fd, uint64_t size) {
    if (fd < 0) {
        Logger::error("Invalid file descriptor for preallocation");
        return false;
    }
    
    // Используем fallocate для предварительного выделения места
    // Это предотвращает фрагментацию и улучшает производительность записи
    if (fallocate(fd, 0, 0, size) != 0) {
        // Fallback: ftruncate (менее эффективно, но работает на всех ФС)
        if (ftruncate(fd, size) != 0) {
            Logger::warning(std::format("Failed to preallocate file: {}", strerror(errno)));
            return false;
        }
    }
    
    Logger::debug(std::format("Preallocated {} bytes on FD {}", size, fd));
    return true;
}

bool PerformanceOptimizer::advise_file_access(int fd, bool sequential, bool no_reuse, bool write_once) {
    if (fd < 0) {
        Logger::error("Invalid file descriptor for advice");
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
        Logger::debug(std::format("posix_fadvise failed (non-critical): {}", strerror(errno)));
        return false;
    }
    
    Logger::debug(std::format("File access advice set: sequential={}, no_reuse={}, write_once={}", 
                              sequential, no_reuse, write_once));
    return true;
}

int PerformanceOptimizer::get_cpu_count() {
    int count = sysconf(_SC_NPROCESSORS_ONLN);
    return (count > 0) ? count : 1;
}

int PerformanceOptimizer::get_optimal_io_threads() {
    if (io_thread_count_ > 0) {
        return io_thread_count_;
    }
    return get_cpu_count();
}

int PerformanceOptimizer::get_optimal_cpu_threads() {
    if (cpu_thread_count_ > 0) {
        return cpu_thread_count_;
    }
    int cpu_count = get_cpu_count();
    return std::max(1, cpu_count - 1);
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

bool PerformanceOptimizer::is_io_uring_available() {
    // Проверяем наличие io_uring через попытку открытия /dev/io_uring
    // или через проверку версии ядра (io_uring доступен с ядра 5.1)
    struct stat st;
    if (stat("/dev/io_uring", &st) == 0) {
        return true;
    }
    
    // Альтернативно: проверяем версию ядра
    std::ifstream version("/proc/sys/kernel/osrelease");
    if (version.is_open()) {
        std::string ver;
        std::getline(version, ver);
        
        // Парсим версию ядра (маajor.minor.patch)
        int major = 0, minor = 0;
        char dot;
        std::istringstream iss(ver);
        if (iss >> major >> dot >> minor) {
            // io_uring доступен с ядра 5.1
            if (major > 5 || (major == 5 && minor >= 1)) {
                return true;
            }
        }
    }
    
    return false;
}

size_t PerformanceOptimizer::get_page_size() {
    if (page_size_ == 0) {
        page_size_ = sysconf(_SC_PAGESIZE);
        if (page_size_ <= 0) {
            page_size_ = 4096;
        }
    }
    return page_size_;
}

size_t PerformanceOptimizer::get_huge_page_size() {
    return huge_page_size_;
}
