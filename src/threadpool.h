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
    ThreadPool(size_t threads, size_t max_queue_size = 1000) 
        : stop_flag(false), max_queue_size(max_queue_size), active_tasks(0) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop_flag || !tasks.empty(); });
                        if (stop_flag && tasks.empty()) return;
                        task = std::move(tasks.top().task);
                        tasks.pop();
                        ++active_tasks;
                    }
                    task();
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        --active_tasks;
                        task_done.notify_all();
                    }
                }
            });
        }
    }
    ~ThreadPool() { stop(); }
    
    bool enqueue(std::function<void()> task, TaskPriority priority = TaskPriority::NORMAL) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
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
        stop_flag = true;
        condition.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }
    
    size_t queue_size() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex));
        return tasks.size();
    }
    
    size_t active_count() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex));
        return active_tasks;
    }
    
    // Ожидание завершения всех задач
    void wait_all() {
        std::unique_lock<std::mutex> lock(queue_mutex);
        task_done.wait(lock, [this] { 
            return tasks.empty() && active_tasks == 0; 
        });
    }
    
private:
    std::vector<std::thread> workers;
    std::priority_queue<PrioritizedTask> tasks;
    mutable std::mutex queue_mutex;
    std::condition_variable condition;
    std::condition_variable task_done;
    std::atomic<bool> stop_flag;
    size_t max_queue_size;
    size_t active_tasks;
};
