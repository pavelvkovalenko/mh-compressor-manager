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
                // Устанавливаем CPU affinity для каждого потока если возможно
                if (i < static_cast<size_t>(PerformanceOptimizer::get_cpu_count())) {
                    PerformanceOptimizer::set_cpu_affinity(static_cast<int>(i));
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

                            // Если таймаут истек, проверяем не нужно ли завершаться
                            if (!wait_result && !stop_flag) {
                                Logger::warning("I/O slot wait timeout, possible deadlock detected");
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
                // Перемещаем поток в локальную переменную
                std::thread t = std::move(worker);

                // Запускаем join в detached-потоке
                std::atomic<bool>* join_done = new std::atomic<bool>(false);
                std::thread joiner([t = std::move(t), join_done]() mutable {
                    t.join();
                    join_done->store(true);
                    delete join_done;
                });
                joiner.detach();

                // Ждём завершения с таймаутом
                auto start = std::chrono::steady_clock::now();
                while (!join_done->load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    if (elapsed > THREAD_JOIN_TIMEOUT) {
                        Logger::warning("Thread join timeout, thread may be stuck in uninterruptible wait");
                        break;
                    }
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
