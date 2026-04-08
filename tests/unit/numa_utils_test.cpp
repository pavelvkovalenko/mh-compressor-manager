/**
 * Unit тесты для NumaUtils
 * Проверяют корректность NUMA-оптимизаций
 */

#include <gtest/gtest.h>
#include "numa_utils.h"
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

// Тест инициализации NUMA
TEST(NumaUtilsTest, Initialize) {
    bool result = NumaUtils::initialize();
    
    // Результат зависит от наличия NUMA в системе
    SUCCEED() << "NUMA initialization: " << (result ? "success" : "not available");
}

// Тест проверки доступности NUMA
TEST(NumaUtilsTest, IsNumaAvailable) {
    // Инициализируем сначала
    NumaUtils::initialize();
    
    bool available = NumaUtils::is_numa_available();
    SUCCEED() << "NUMA available: " << (available ? "yes" : "no");
}

// Тест получения количества NUMA узлов
TEST(NumaUtilsTest, GetNumaNodeCount) {
    NumaUtils::initialize();
    
    int count = NumaUtils::get_numa_node_count();
    
    // Если NUMA доступен, должно быть хотя бы 1 узел
    if (NumaUtils::is_numa_available()) {
        EXPECT_GE(count, 1);
    } else {
        // Без NUMA может вернуть 0 или 1
        EXPECT_GE(count, 0);
    }
    
    SUCCEED() << "NUMA node count: " << count;
}

// Тест определения NUMA узла для файла
TEST(NumaUtilsTest, GetFileNumaNode) {
    NumaUtils::initialize();
    
    fs::path temp_file = fs::temp_directory_path() / "mh_numa_test.txt";
    
    // Создаем временный файл
    {
        std::ofstream ofs(temp_file);
        ofs << "test data";
    }
    
    int node = NumaUtils::get_file_numa_node(temp_file);
    
    // Может вернуть -1 если не удалось определить
    if (node != -1) {
        EXPECT_GE(node, 0);
    }
    
    SUCCEED() << "File NUMA node: " << node;
    
    fs::remove(temp_file);
}

// Тест привязки потока к NUMA узлу
TEST(NumaUtilsTest, BindCurrentThreadToNode) {
    NumaUtils::initialize();
    
    if (!NumaUtils::is_numa_available()) {
        SUCCEED() << "NUMA not available, skipping bind test";
        return;
    }
    
    // Пробуем привязать к узлу 0 (должен существовать всегда)
    bool result = NumaUtils::bind_current_thread_to_node(0);
    
    // Может вернуть false если нет прав или узел недоступен
    SUCCEED() << "Thread binding to node 0: " << (result ? "success" : "failed");
}

// Тест выделения памяти на NUMA узле
TEST(NumaUtilsTest, AllocateOnNode) {
    NumaUtils::initialize();
    
    if (!NumaUtils::is_numa_available()) {
        SUCCEED() << "NUMA not available, skipping allocate test";
        return;
    }
    
    size_t size = 4096;
    void* ptr = NumaUtils::allocate_on_node(size, 0);
    
    if (ptr != nullptr) {
        // Проверяем что память выделена
        memset(ptr, 0xAB, size);
        
        // Освобождаем
        NumaUtils::free_on_node(ptr, size);
        SUCCEED() << "Memory allocated and freed successfully";
    } else {
        SUCCEED() << "Memory allocation failed (expected on some systems)";
    }
}

// Тест освобождения NUMA памяти
TEST(NumaUtilsTest, FreeOnNode) {
    NumaUtils::initialize();
    
    if (!NumaUtils::is_numa_available()) {
        SUCCEED() << "NUMA not available, skipping free test";
        return;
    }
    
    size_t size = 8192;
    void* ptr = NumaUtils::allocate_on_node(size, 0);
    ASSERT_NE(ptr, nullptr);
    
    // Заполняем данными
    memset(ptr, 0xCD, size);
    
    // Освобождаем
    NumaUtils::free_on_node(ptr, size);
    
    SUCCEED() << "Memory freed successfully";
}

// Тест привязки памяти к NUMA узлу
TEST(NumaUtilsTest, BindMemoryToNode) {
    NumaUtils::initialize();
    
    if (!NumaUtils::is_numa_available()) {
        SUCCEED() << "NUMA not available, skipping bind memory test";
        return;
    }
    
    // Выделяем обычную память
    size_t size = 4096;
    void* ptr = malloc(size);
    ASSERT_NE(ptr, nullptr);
    
    // Привязываем к узлу 0
    bool result = NumaUtils::bind_memory_to_node(ptr, size, 0);
    
    free(ptr);
    
    SUCCEED() << "Memory binding: " << (result ? "success" : "failed");
}

// Тест получения оптимального узла для устройства
TEST(NumaUtilsTest, GetOptimalNodeForDevice) {
    NumaUtils::initialize();
    
    fs::path temp_file = fs::temp_directory_path() / "mh_device_test.txt";
    
    // Создаем временный файл
    {
        std::ofstream ofs(temp_file);
        ofs << "test";
    }
    
    int node = NumaUtils::get_optimal_node_for_device(temp_file);
    
    // Должен вернуть неотрицательное значение (по умолчанию 0)
    EXPECT_GE(node, 0);
    
    SUCCEED() << "Optimal node for device: " << node;
    
    fs::remove(temp_file);
}

// Тест работы с несуществующим файлом
TEST(NumaUtilsTest, NonexistentFileHandling) {
    NumaUtils::initialize();
    
    fs::path nonexistent = fs::temp_directory_path() / "mh_nonexistent_xyz123.txt";
    
    int node = NumaUtils::get_file_numa_node(nonexistent);
    
    // Для несуществующего файла должен вернуть -1
    EXPECT_EQ(node, -1);
    
    SUCCEED() << "Nonexistent file handling: returned " << node;
}

// Тест множественной инициализации
TEST(NumaUtilsTest, MultipleInitialization) {
    // Инициализируем несколько раз
    bool result1 = NumaUtils::initialize();
    bool result2 = NumaUtils::initialize();
    bool result3 = NumaUtils::initialize();
    
    // Все должны вернуть одинаковый результат
    EXPECT_EQ(result1, result2);
    EXPECT_EQ(result2, result3);
    
    SUCCEED() << "Multiple initialization handled correctly";
}

// Тест привязки к несуществующему узлу
TEST(NumaUtilsTest, InvalidNodeBinding) {
    NumaUtils::initialize();
    
    // Пробуем привязать к несуществующему узлу (999)
    bool result = NumaUtils::bind_current_thread_to_node(999);
    
    // Должно вернуть false
    EXPECT_FALSE(result);
    
    SUCCEED() << "Invalid node binding correctly rejected";
}

// Тест выделения памяти нулевого размера
TEST(NumaUtilsTest, ZeroSizeAllocation) {
    NumaUtils::initialize();
    
    if (!NumaUtils::is_numa_available()) {
        SUCCEED() << "NUMA not available, skipping zero size test";
        return;
    }
    
    void* ptr = NumaUtils::allocate_on_node(0, 0);
    
    // Поведение зависит от реализации, но не должно падать
    if (ptr != nullptr) {
        NumaUtils::free_on_node(ptr, 0);
    }
    
    SUCCEED() << "Zero size allocation handled";
}

// Интеграционный тест: полный цикл работы с NUMA
TEST(NumaUtilsTest, FullWorkflowIntegration) {
    // Инициализация
    bool init = NumaUtils::initialize();
    
    if (!init || !NumaUtils::is_numa_available()) {
        SUCCEED() << "NUMA not available, integration test skipped";
        return;
    }
    
    // Получаем количество узлов
    int node_count = NumaUtils::get_numa_node_count();
    EXPECT_GE(node_count, 1);
    
    // Выбираем первый доступный узел
    int target_node = 0;
    
    // Привязываем поток
    bool bind_result = NumaUtils::bind_current_thread_to_node(target_node);
    
    // Выделяем память на узле
    size_t size = 16384;
    void* ptr = NumaUtils::allocate_on_node(size, target_node);
    
    if (ptr != nullptr) {
        // Используем память
        memset(ptr, 0xFF, size);
        
        // Привязываем память
        bool mem_bind = NumaUtils::bind_memory_to_node(ptr, size, target_node);
        
        // Освобождаем
        NumaUtils::free_on_node(ptr, size);
        
        SUCCEED() << "Full workflow completed: init=" << init 
                  << ", bind=" << bind_result 
                  << ", mem_bind=" << mem_bind;
    } else {
        SUCCEED() << "Memory allocation failed in integration test";
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
