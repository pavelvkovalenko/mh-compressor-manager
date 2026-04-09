cmake_minimum_required(VERSION 3.20)
project(mh-compressor-manager-tests LANGUAGES CXX)

# ============================================================================
# Стандарт C++23 (должен соответствовать основному проекту)
# ============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Поиск GoogleTest
# ============================================================================
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Для Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ============================================================================
# Поиск зависимостей (те же что и в основном проекте)
# ============================================================================
find_package(PkgConfig REQUIRED)

pkg_check_modules(ZLIB REQUIRED zlib)
pkg_check_modules(BROTLI REQUIRED libbrotlienc libbrotlicommon)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)

# Поиск fmt library (как в основном проекте)
find_package(fmt QUIET)
if(NOT fmt_FOUND)
    # Если fmt не найден как package, используем pkg-config
    pkg_check_modules(FMT QUIET fmt)
endif()

# ============================================================================
# Исходные файлы тестов
# ============================================================================

# Unit тесты для memory_pool
add_executable(memory_pool_test
    unit/memory_pool_test.cpp
    ../src/numa_utils.cpp
    ../src/performance_optimizer.cpp
    ../src/logger.cpp
)
target_include_directories(memory_pool_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${ZLIB_INCLUDE_DIRS}
    ${BROTLI_INCLUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
)
target_link_libraries(memory_pool_test PRIVATE
    GTest::gtest_main
    pthread
    numa
    fmt
)
target_compile_definitions(memory_pool_test PRIVATE HAVE_NUMA=1)
target_compile_options(memory_pool_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(memory_pool_test PRIVATE
    --coverage
)

# Unit тесты для security функций
add_executable(security_test
    unit/security_test.cpp
    ../src/security.cpp
    ../src/logger.cpp
)
target_include_directories(security_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${ZLIB_INCLUDE_DIRS}
    ${BROTLI_INCLUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
)
target_link_libraries(security_test PRIVATE
    GTest::gtest_main
    pthread
    ${SYSTEMD_LIBRARIES}
    seccomp
    cap
)
target_compile_options(security_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(security_test PRIVATE
    --coverage
)

# Unit тесты для config
add_executable(config_test
    unit/config_test.cpp
    ../src/config.cpp
    ../src/logger.cpp
)
target_include_directories(config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${ZLIB_INCLUDE_DIRS}
    ${BROTLI_INCLUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
)
target_link_libraries(config_test PRIVATE
    GTest::gtest_main
    pthread
    ${SYSTEMD_LIBRARIES}
    fmt
)
target_compile_options(config_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(config_test PRIVATE
    --coverage
)

# Unit тесты для compressor (с моками)
add_executable(compressor_test
    unit/compressor_test.cpp
    ../src/compressor.cpp
    ../src/logger.cpp
    ../src/async_io.cpp
    ../src/numa_utils.cpp
    ../src/performance_optimizer.cpp
)
target_include_directories(compressor_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${ZLIB_INCLUDE_DIRS}
    ${BROTLI_INCLUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
)
target_link_libraries(compressor_test PRIVATE
    GTest::gtest_main
    pthread
    ${ZLIB_LIBRARIES}
    ${BROTLI_LIBRARIES}
    ${SYSTEMD_LIBRARIES}
    numa
    uring
    fmt
)
target_compile_definitions(compressor_test PRIVATE HAVE_NUMA=1)
target_compile_options(compressor_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(compressor_test PRIVATE
    --coverage
)

# Unit тесты для logger
add_executable(logger_test
    unit/logger_test.cpp
    ../src/logger.cpp
)
target_include_directories(logger_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
)
target_link_libraries(logger_test PRIVATE
    GTest::gtest_main
    pthread
    ${SYSTEMD_LIBRARIES}
)
target_compile_options(logger_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(logger_test PRIVATE
    --coverage
)

# Unit тесты для threadpool
add_executable(threadpool_test
    unit/threadpool_test.cpp
)
target_include_directories(threadpool_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
)
target_link_libraries(threadpool_test PRIVATE
    GTest::gtest_main
    pthread
)
target_compile_options(threadpool_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(threadpool_test PRIVATE
    --coverage
)

# Unit тесты для async_io
add_executable(async_io_test
    unit/async_io_test.cpp
    ../src/async_io.cpp
    ../src/logger.cpp
    ../src/numa_utils.cpp
    ../src/performance_optimizer.cpp
)
target_include_directories(async_io_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${ZLIB_INCLUDE_DIRS}
    ${BROTLI_INCLUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
)
target_link_libraries(async_io_test PRIVATE
    GTest::gtest_main
    pthread
    ${SYSTEMD_LIBRARIES}
    numa
    uring
    fmt
)
target_compile_definitions(async_io_test PRIVATE HAVE_NUMA=1)
target_compile_options(async_io_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(async_io_test PRIVATE
    --coverage
)

# Unit тесты для numa_utils
add_executable(numa_utils_test
    unit/numa_utils_test.cpp
    ../src/numa_utils.cpp
    ../src/logger.cpp
)
target_include_directories(numa_utils_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${SYSTEMD_INCLUDE_DIRS}
)
target_link_libraries(numa_utils_test PRIVATE
    GTest::gtest_main
    pthread
    ${SYSTEMD_LIBRARIES}
    numa
)
target_compile_options(numa_utils_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(numa_utils_test PRIVATE
    --coverage
)

# Unit тесты для simd_utils
add_executable(simd_utils_test
    unit/simd_utils_test.cpp
)
target_include_directories(simd_utils_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
)
target_link_libraries(simd_utils_test PRIVATE
    GTest::gtest_main
    pthread
)
target_compile_options(simd_utils_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage -mavx2
)
target_link_options(simd_utils_test PRIVATE
    --coverage
)

# ============================================================================
# Интеграционные тесты
# ============================================================================

# Интеграционный тест для end-to-end проверки сжатия
add_executable(integration_test
    integration/integration_test.cpp
    ../src/compressor.cpp
    ../src/logger.cpp
    ../src/config.cpp
    ../src/async_io.cpp
    ../src/security.cpp
)
target_include_directories(integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${ZLIB_INCLUDE_DIRS}
    ${BROTLI_INCLUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
)
target_link_libraries(integration_test PRIVATE
    GTest::gtest_main
    pthread
    ${ZLIB_LIBRARIES}
    ${BROTLI_LIBRARIES}
    ${SYSTEMD_LIBRARIES}
)
target_compile_options(integration_test PRIVATE
    -Wall -Wextra -Wpedantic -g -fprofile-arcs -ftest-coverage
)
target_link_options(integration_test PRIVATE
    --coverage
)

# ============================================================================
# Регистрация тестов в CTest
# ============================================================================
enable_testing()

include(GoogleTest)
gtest_discover_tests(memory_pool_test)
gtest_discover_tests(security_test)
gtest_discover_tests(config_test)
gtest_discover_tests(compressor_test)
gtest_discover_tests(logger_test)
gtest_discover_tests(threadpool_test)
gtest_discover_tests(async_io_test)
gtest_discover_tests(numa_utils_test)
gtest_discover_tests(simd_utils_test)
gtest_discover_tests(integration_test)

# ============================================================================
# Целевые задачи для запуска тестов и покрытия
# ============================================================================
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS memory_pool_test security_test config_test compressor_test logger_test threadpool_test async_io_test numa_utils_test simd_utils_test integration_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all tests"
)

add_custom_target(coverage_report
    COMMAND mkdir -p coverage
    COMMAND lcov --capture --directory . --output-file coverage/coverage.info --gcov-tool gcov
    COMMAND genhtml coverage/coverage.info --output-directory coverage/html
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating coverage report"
)

message(STATUS "")
message(STATUS "=== mh-compressor-manager Test Build Configuration ===")
message(STATUS \"CMake Version:        ${CMAKE_VERSION}\")
message(STATUS \"C++ Standard:         ${CMAKE_CXX_STANDARD}\")
message(STATUS \"Compiler:             ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\")
message(STATUS \"Build Type:           ${CMAKE_BUILD_TYPE}\")
message(STATUS \"GoogleTest:           FetchContent (v1.14.0)\")
message(STATUS \"============================================\")
message(STATUS "")
