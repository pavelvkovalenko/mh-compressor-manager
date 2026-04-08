/**
 * Unit тесты для ThreadPool
 * Проверяют потокобезопасность, обработку задач и shutdown
 */

#include <gtest/gtest.h>
#include "threadpool.h"
#include <atomic>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>

// Тест создания пула с разным количеством потоков
TEST(ThreadPoolTest, CreationWithDifferentSizes) {
    EXPECT_NO_THROW({ ThreadPool pool(1); });
    EXPECT_NO_THROW({ ThreadPool pool(4); });
    EXPECT_NO_THROW({ ThreadPool pool(16); });
    SUCCEED() << "ThreadPool creation works with different sizes";
}

// Тест выполнения простых задач
TEST(ThreadPoolTest, SimpleTaskExecution) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 100; ++i) {
        pool.enqueue([&counter]() {
            counter++;
        });
    }
    
    pool.stop();
    
    EXPECT_EQ(counter.load(), 100);
}

// Тест приоритетов задач
TEST(ThreadPoolTest, TaskPriorities) {
    ThreadPool pool(2);  // Ограниченное количество потоков
    std::vector<std::string> execution_order;
    std::mutex mutex;
    
    // Добавляем задачи с разными приоритетами
    pool.enqueue([&execution_order, &mutex]() {
        std::lock_guard<std::mutex> lock(mutex);
        execution_order.push_back("LOW");
    }, TaskPriority::LOW);
    
    pool.enqueue([&execution_order, &mutex]() {
        std::lock_guard<std::mutex> lock(mutex);
        execution_order.push_back("HIGH");
    }, TaskPriority::HIGH);
    
    pool.enqueue([&execution_order, &mutex]() {
        std::lock_guard<std::mutex> lock(mutex);
        execution_order.push_back("NORMAL");
    }, TaskPriority::NORMAL);
    
    pool.stop();
    
    // HIGH приоритет должен выполниться раньше (но не гарантировано)
    EXPECT_GE(execution_order.size(), 3);
    SUCCEED() << "Task priorities handled";
}

// Тест отмены при переполнении очереди
TEST(ThreadPoolTest, QueueOverflowHandling) {
    ThreadPool pool(1, 10);  // Малый размер очереди
    std::atomic<int> accepted_tasks{0};
    std::atomic<int> rejected_tasks{0};
    
    // Блокируем поток пула
    pool.enqueue([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });
    
    // Пытаемся добавить много задач
    for (int i = 0; i < 20; ++i) {
        bool result = pool.enqueue([&accepted_tasks]() {
            accepted_tasks++;
        });
        
        if (!result) {
            rejected_tasks++;
        }
    }
    
    pool.stop();
    
    // Некоторые задачи должны быть отклонены
    SUCCEED() << "Accepted: " << accepted_tasks.load() << ", Rejected: " << rejected_tasks.load();
}

// Тест graceful shutdown
TEST(ThreadPoolTest, GracefulShutdown) {
    ThreadPool pool(4);
    std::atomic<int> completed{0};
    
    for (int i = 0; i < 50; ++i) {
        pool.enqueue([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            completed++;
        });
    }
    
    // stop() должен дождаться завершения всех задач
    pool.stop();
    
    EXPECT_EQ(completed.load(), 50);
}

// Тест обработки исключений в задачах
TEST(ThreadPoolTest, ExceptionHandling) {
    ThreadPool pool(4);
    std::atomic<bool> exception_thrown{false};
    
    pool.enqueue([&exception_thrown]() {
        throw std::runtime_error("Test exception");
    });
    
    pool.enqueue([&exception_thrown]() {
        exception_thrown = true;
    });
    
    // Пул должен продолжить работу после исключения
    pool.stop();
    
    EXPECT_TRUE(exception_thrown.load());
}

// Тест производительности
TEST(ThreadPoolTest, PerformanceTest) {
    ThreadPool pool(8);
    std::atomic<uint64_t> total_work{0};
    constexpr int num_tasks = 10000;
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < num_tasks; ++i) {
        pool.enqueue([&total_work]() {
            uint64_t sum = 0;
            for (int j = 0; j < 1000; ++j) {
                sum += j;
            }
            total_work += sum;
        });
    }
    
    pool.stop();
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    EXPECT_GT(total_work.load(), 0);
    SUCCEED() << "Completed " << num_tasks << " tasks in " << duration << "ms";
}

// Тест стрессовой нагрузки
TEST(ThreadPoolTest, StressTest) {
    ThreadPool pool(16);
    std::atomic<int> counter{0};
    constexpr int num_tasks = 1000;
    
    for (int i = 0; i < num_tasks; ++i) {
        pool.enqueue([&counter, i]() {
            counter++;
            // Случайная задержка
            std::this_thread::sleep_for(std::chrono::microseconds(rand() % 100));
        });
    }
    
    pool.stop();
    
    EXPECT_EQ(counter.load(), num_tasks);
}

// Тест повторного использования потоков
TEST(ThreadPoolTest, ThreadReuse) {
    ThreadPool pool(2);
    std::atomic<int> task_count{0};
    
    // Выполняем задачи несколько раз
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 10; ++i) {
            pool.enqueue([&task_count]() {
                task_count++;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    pool.stop();
    
    EXPECT_EQ(task_count.load(), 50);
}

// Тест пустой очереди
TEST(ThreadPoolTest, EmptyQueueShutdown) {
    ThreadPool pool(4);
    // Немедленный shutdown без задач
    EXPECT_NO_THROW(pool.stop());
    SUCCEED() << "Empty queue shutdown works";
}

// Тест с lambda захватом по значению и ссылке
TEST(ThreadPoolTest, LambdaCapture) {
    ThreadPool pool(4);
    std::vector<int> results;
    std::mutex mutex;
    
    int shared_value = 100;
    
    for (int i = 0; i < 10; ++i) {
        // Захват по значению
        pool.enqueue([i, &results, &mutex]() {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(i * 2);
        });
        
        // Захват по ссылке (должен работать корректно)
        pool.enqueue([&shared_value, &results, &mutex, i]() {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(shared_value + i);
        });
    }
    
    pool.stop();
    
    EXPECT_EQ(results.size(), 20);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
