# mh-compressor-manager Tests

## Структура тестов

```
tests/
├── CMakeLists.txt          # Конфигурация сборки тестов
├── README.md               # Этот файл
├── unit/                   # Unit тесты
│   ├── memory_pool_test.cpp    # Тесты пула памяти
│   ├── security_test.cpp       # Тесты безопасности
│   ├── config_test.cpp         # Тесты конфигурации
│   ├── compressor_test.cpp     # Тесты компрессора
│   ├── logger_test.cpp         # Тесты логгера
│   └── threadpool_test.cpp     # Тесты пула потоков
├── integration/            # Интеграционные тесты
│   └── integration_test.cpp    # End-to-end тесты
└── mocks/                  # Моки для тестирования
```

## Сборка и запуск тестов

### Требования
- CMake 3.20+
- GCC 13+ или Clang 16+
- GoogleTest (загружается автоматически через FetchContent)
- zlib, brotli, libsystemd (те же зависимости что и у основного проекта)

### Сборка

```bash
cd tests
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Запуск всех тестов

```bash
ctest --output-on-failure
```

### Запуск отдельных тестов

```bash
# Запустить все тесты memory_pool
./memory_pool_test

# Запустить конкретный тест
./memory_pool_test --gtest_filter="MemoryPoolTest.ThreadSafety"

# Запустить тесты с паттерном
./compressor_test --gtest_filter="*Security*"
```

### Генерация отчета о покрытии кода

```bash
make coverage_report
# Отчет будет в build/coverage/html/index.html
```

## Описание тестов

### Unit тесты

#### MemoryPoolTest
- BasicAllocateRelease - базовое выделение/освобождение
- MultipleAllocations - множественные выделения
- BufferReuse - повторное использование буферов
- ThreadSafety - потокобезопасность
- ExhaustionHandling - обработка исчерпания памяти
- DoubleFreeProtection - защита от double-free
- VectorSizeValidation - валидация размера vector
- MemoryAlignment - проверка выравнивания
- StressTest - стрессовая нагрузка

#### SecurityTest
- IsRunningAsRoot - проверка запуска от root
- SymlinkDetection - обнаружение symlink атак
- InvalidUserHandling - обработка несуществующих пользователей
- EmptyPathsHandling - обработка пустых путей
- SeccompAvailability - проверка доступности seccomp

#### ConfigTest
- BasicConfigLoad - базовая загрузка конфигурации
- NonExistentConfigFile - несуществующий файл конфигурации
- InvalidCompressionLevels - невалидные уровни сжатия
- EmptyTargetPaths - пустой список путей
- SymlinkInPaths - symlink в путях
- SpecialCharactersInPaths - специальные символы

#### CompressorTest
- GzipBasicCompression - базовое сжатие gzip
- BrotliBasicCompression - базовое сжатие brotli
- SymlinkRejection - отказ от сжатия symlink
- NoReadPermission - отсутствие прав на чтение
- LargeFileCompression - сжатие больших файлов
- CompressionLevels - различные уровни сжатия

#### LoggerTest
- InfoLogging - логирование info
- WarningLogging - логирование warning
- ErrorLogging - логирование error
- MultithreadedLogging - многопоточное логирование
- PerformanceLogging - производительность

#### ThreadPoolTest
- SimpleTaskExecution - выполнение простых задач
- TaskPriorities - приоритеты задач
- QueueOverflowHandling - переполнение очереди
- GracefulShutdown - корректная остановка
- ExceptionHandling - обработка исключений
- StressTest - стрессовая нагрузка

### Интеграционные тесты

#### IntegrationTest
- FullCompressionCycle - полный цикл сжатия
- DualAlgorithmCompression - сжатие обоими алгоритмами
- SymlinkAttackPrevention - предотвращение symlink атак
- BulkFileCompression - пакетное сжатие файлов
- LargeFileHandling - обработка больших файлов
- ErrorHandlingNonExistentFiles - обработка ошибок

## Безопасность тестов

Все тесты разработаны с учетом следующих принципов безопасности:

1. **Изоляция**: Тесты используют временные директории в `/tmp`
2. **Очистка**: Все созданные файлы удаляются после тестов (TearDown)
3. **Валидация**: Тесты проверяют обработку невалидных входных данных
4. **Symlink атаки**: Специальные тесты проверяют защиту от symlink атак
5. **Права доступа**: Тесты проверяют обработку отсутствующих прав

## Добавление новых тестов

1. Создайте новый файл в `unit/` или `integration/`
2. Добавьте исполняемый файл в `CMakeLists.txt`
3. Зарегистрируйте тесты через `gtest_discover_tests()`

Пример нового теста:
```cpp
#include <gtest/gtest.h>

TEST(MyTestSuite, MyTestCase) {
    EXPECT_EQ(2 + 2, 4);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

## CI/CD интеграция

Для GitHub Actions добавьте шаг:
```yaml
- name: Build and run tests
  run: |
    cd tests
    mkdir build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Debug
    make -j$(nproc)
    ctest --output-on-failure
```
