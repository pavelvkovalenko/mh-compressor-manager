#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <csignal>
#include <cstring>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "logger.h"
#include "performance_optimizer.h"

// Приоритеты задач
enum class TaskPriority : uint8_t {
    LOW = 0,
    NORMAL = 1,
    HIGH = 2
};

struct PrioritizedTask {
    std::function<void()> task;
    TaskPriority priority;
    std::chrono::steady_clock::time_point enqueue_time;
    uint64_t file_size;  // Кэшированный размер файла для оптимизации
    
    PrioritizedTask() : priority(TaskPriority::NORMAL), file_size(0) {}
    PrioritizedTask(std::function<void()> t, TaskPriority p, 
                   std::chrono::steady_clock::time_point time, uint64_t size = 0)
        : task(std::move(t)), priority(p), enqueue_time(time), file_size(size) {}
    
    // Оператор сравнения для priority_queue (больший приоритет = выше в очереди)
    bool operator<(const PrioritizedTask& other) const {
        if (priority != other.priority) {
            return priority < other.priority;  // Больший приоритет выше
        }
        // При одинаковом приоритете - более старые задачи выполняются первыми (FIFO)
        return enqueue_time > other.enqueue_time;
    }
};

class ThreadPool {
public:
    ThreadPool(size_t threads, size_t max_queue_size = 1000, size_t max_ios = 0)
        : stop_flag(false), stopped(false), max_queue_size(max_queue_size), active_tasks(0),
          m_max_active_ios(max_ios), io_slots_available(max_ios > 0 ? max_ios : SIZE_MAX) {
        // Резервируем память с учетом кэш-линий для предотвращения false sharing
        workers.reserve(threads);
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this, i] {
                // Устанавливаем CPU affinity для каждого потока с циклическим распределением (ТЗ §3.2.8)
                {
                    size_t cpu_count = static_cast<size_t>(PerformanceOptimizer::get_cpu_count());
                    if (cpu_count == 0) cpu_count = 1;  // Защита от деления на 0
                    int core_id = static_cast<int>(i % cpu_count);
                    if (!PerformanceOptimizer::set_cpu_affinity(core_id)) {
                        Logger::warning(std::format("Failed to set CPU affinity for thread {} (core {})", i, core_id));
                    }
                }

                // Понижаем приоритет CPU (nice=10) — фоновые задачи уступают nginx и другим важным процессам
                if (setpriority(PRIO_PROCESS, 0, 10) == 0) {
                    Logger::debug(std::format("Worker thread {} CPU nice set to 10 (lower priority)", i));
                } else {
                    Logger::debug(std::format("Failed to set CPU nice for worker {}: {}", i, strerror(errno)));
                }

                // Понижаем I/O приоритет (idle class = 3) — минимальное влияние на диск
                // ioprio_set(IOPRIO_WHO_PROCESS, 0, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0))
                constexpr int IOPRIO_CLASS_IDLE = 3;
                constexpr int IOPRIO_WHO_PROCESS = 1;
                int ioprio = IOPRIO_CLASS_IDLE << 13;  // level 0 within idle class
                if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, ioprio) == 0) {
                    Logger::debug(std::format("Worker thread {} I/O priority set to idle", i));
                } else {
                    Logger::debug(std::format("Failed to set I/O priority for worker {}: {}", i, strerror(errno)));
                }
                
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop_flag || !tasks.empty(); });
                        if (stop_flag && tasks.empty()) return;
                        
                        // Ждем доступного I/O слота если установлен лимит (с таймаутом для предотвращения блокировок)
                        if (m_max_active_ios > 0) {
                            auto wait_result = io_slot_available.wait_for(lock,
                                std::chrono::seconds(5),  // Таймаут 5 секунд для предотвращения deadlock
                                [this] { return io_slots_available > 0 || stop_flag; });

                            // Если таймаут истек и слот всё ещё недоступен — пропускаем задачу
                            if (!wait_result) {
                                if (stop_flag && tasks.empty()) return;
                                Logger::warning("I/O slot wait timeout, requeueing task");
                                // Не берём задачу — возвращаемся к ожиданию
                                continue;
                            }
                            if (stop_flag && tasks.empty()) return;
                        }
                        
                        auto& top = tasks.top();
                        task = std::move(top.task);
                        tasks.pop();
                        ++active_tasks;
                        if (m_max_active_ios > 0) {
                            --io_slots_available;
                        }
                    }
                    
                    // Выполняем задачу вне блокировки
                    task();
                    
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        --active_tasks;
                        if (m_max_active_ios > 0) {
                            ++io_slots_available;
                            io_slot_available.notify_one();
                        }
                        task_done.notify_all();
                    }
                }
            });
        }
    }
    ~ThreadPool() {
        // Безопасно останавливаем только если еще не остановлен
        bool expected = false;
        if (stopped.compare_exchange_strong(expected, true)) {
            stop_internal();
        }
    }

    bool enqueue(std::function<void()> task, TaskPriority priority = TaskPriority::NORMAL) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            // Не принимаем задачи если пул останавливается
            if (stop_flag) {
                return false;
            }
            if (tasks.size() >= max_queue_size) {
                return false;
            }
            PrioritizedTask pt{std::move(task), priority, std::chrono::steady_clock::now()};
            tasks.push(std::move(pt));
        }
        condition.notify_one();
        return true;
    }
    
    void stop() {
        // Безопасно останавливаем только один раз
        bool expected = false;
        if (stopped.compare_exchange_strong(expected, true)) {
            stop_internal();
        }
    }

    size_t queue_size() const {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return tasks.size();
    }

    size_t active_count() const {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return active_tasks;
    }

    // Ожидание завершения всех задач
    void wait_all() {
        std::unique_lock<std::mutex> lock(queue_mutex);
        task_done.wait(lock, [this] {
            return (tasks.empty() && active_tasks == 0) || stop_flag;
        });
    }

private:
    void stop_internal() {
        stop_flag = true;
        condition.notify_all();
        io_slot_available.notify_all();  // Пробуждаем потоки ожидающие I/O слотов

        // Ждем завершения всех задач с таймаутом
        constexpr auto THREAD_JOIN_TIMEOUT = std::chrono::seconds(30);

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                // Перемещаем поток: после move worker не-joinable,
                // workers.clear() безопасен даже если join заблокируется
                std::thread t = std::move(worker);

                // Запускаем мониторинговый поток, который логирует прогресс
                std::atomic<bool> join_done{false};
                std::thread monitor([this, &join_done]() {
                    auto start = std::chrono::steady_clock::now();
                    while (!join_done.load()) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        auto elapsed = std::chrono::steady_clock::now() - start;
                        if (elapsed > std::chrono::seconds(60)) {
                            Logger::error("Thread join timeout (60s), aborting monitor");
                            break;
                        }
                        int secs = static_cast<int>(elapsed.count());
                        if (secs >= 5 && secs % 5 == 0) {
                            Logger::warning(std::format("Thread still running after {}s", secs));
                        }
                    }
                });

                // Join — может заблокироваться, но worker должен завершиться
                // т.к. stop_flag установлен и condition.notify_all() вызван
                t.join();
                join_done = true;

                if (monitor.joinable()) {
                    monitor.join();
                }
            }
        }
        workers.clear();  // Очищаем вектор потоков после завершения
    }

private:
    std::vector<std::thread> workers;
    std::priority_queue<PrioritizedTask> tasks;
    mutable std::mutex queue_mutex;
    std::condition_variable condition;
    std::condition_variable task_done;
    std::condition_variable io_slot_available;
    std::atomic<bool> stop_flag;
    std::atomic<bool> stopped;  // Флаг для предотвращения повторного вызова stop
    size_t max_queue_size;
    size_t active_tasks;
    size_t m_max_active_ios;
    size_t io_slots_available;
};
