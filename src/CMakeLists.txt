cmake_minimum_required(VERSION 3.20)
project(mh-compressor-manager LANGUAGES CXX)

# ============================================================================
# Стандарт C++23 (ТЗ п. 1.2)
# Используем C++20 для совместимости с GCC 12, если <format> недоступен
# ============================================================================
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
    #include <format>
    #include <string>
    int main() {
        auto s = std::format(\"{}\", \"test\");
        return 0;
    }
" HAS_STD_FORMAT)

if(NOT HAS_STD_FORMAT)
    message(STATUS "std::format not available, falling back to fmt library")
    set(CMAKE_CXX_STANDARD 20)
    
    # Сначала пробуем найти через find_package (CONFIG mode)
    find_package(fmt QUIET CONFIG)
    
    # Если не найдено, потребуется PkgConfig
    if(NOT fmt_FOUND)
        # Убеждаемся, что PkgConfig доступен
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FMT QUIET fmt)
        if(FMT_FOUND)
            set(fmt_FOUND TRUE)
        endif()
    endif()
    
    if(fmt_FOUND)
        message(STATUS "Using fmt library for formatting")
    else()
        message(FATAL_ERROR 
            "fmt library not found and std::format not available!\n"
            "Please install fmt development package:\n"
            "  - Fedora/RHEL: sudo dnf install fmt-devel\n"
            "  - Debian/Ubuntu: sudo apt-get install libfmt-dev\n"
            "  - openSUSE: sudo zypper install fmt-devel\n"
            "Or use GCC 13+ / Clang 16+ with C++23 support for std::format"
        )
    endif()
else()
    set(CMAKE_CXX_STANDARD 23)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Поиск зависимостей через pkg-config (ТЗ п. 10.2)
# ============================================================================
find_package(PkgConfig REQUIRED)

# zlib - сжатие Gzip
pkg_check_modules(ZLIB QUIET zlib)
if(NOT ZLIB_FOUND)
    message(FATAL_ERROR 
        "zlib library not found!\n"
        "Please install zlib development package:\n"
        "  - Debian/Ubuntu: sudo apt-get install zlib1g-dev\n"
        "  - RHEL/CentOS/Fedora: sudo dnf install zlib-devel\n"
        "  - openSUSE: sudo zypper install zlib-devel"
    )
endif()

# Brotli - сжатие Brotli (нужны обе библиотеки: common и enc)
pkg_check_modules(BROTLI QUIET libbrotlienc libbrotlicommon)
if(NOT BROTLI_FOUND)
    message(FATAL_ERROR 
        "Brotli library not found!\n"
        "Please install Brotli development packages:\n"
        "  - Debian/Ubuntu: sudo apt-get install libbrotli-dev\n"
        "  - RHEL/CentOS/Fedora: sudo dnf install brotli-devel\n"
        "  - openSUSE: sudo zypper install libbrotli-devel"
    )
endif()

# systemd - интеграция с systemd (sd_notify)
pkg_check_modules(SYSTEMD QUIET libsystemd)
if(NOT SYSTEMD_FOUND)
    message(FATAL_ERROR 
        "systemd library not found!\n"
        "Please install systemd development package:\n"
        "  - Debian/Ubuntu: sudo apt-get install libsystemd-dev\n"
        "  - RHEL/CentOS/Fedora: sudo dnf install systemd-devel\n"
        "  - openSUSE: sudo zypper install libsystemd-devel"
    )
endif()

# SELinux - опциональная поддержка контекстов безопасности (ТЗ п. 2.5)
pkg_check_modules(SELINUX libselinux)

# Seccomp - песочница системных вызовов
pkg_check_modules(SECCOMP libseccomp)

# Libcap - управление capabilities (для сброса привилегий)
pkg_check_modules(LIBCAP libcap)
if(LIBCAP_FOUND)
    message(STATUS "Libcap support enabled")
else()
    message(STATUS "Libcap support disabled (libcap not found)")
endif()

# io_uring - асинхронный I/O (опционально)
pkg_check_modules(LIBURING liburing)
if(LIBURING_FOUND)
    message(STATUS "io_uring support enabled")
    set(HAS_IO_URING 1)
else()
    message(STATUS "io_uring support disabled (liburing not found), using sync I/O fallback")
    set(HAS_IO_URING 0)
endif()

# NUMA - оптимизация для многопроцессорных систем (опционально)
pkg_check_modules(NUMA numa)
if(NUMA_FOUND)
    message(STATUS "NUMA support enabled")
    set(HAS_NUMA 1)
else()
    message(STATUS "NUMA support disabled (libnuma not found)")
    set(HAS_NUMA 0)
endif()

# ============================================================================
# Исходные файлы
# ============================================================================
set(SOURCES
    main.cpp
    config.cpp
    compressor.cpp
    monitor.cpp
    logger.cpp
    security.cpp
    async_io.cpp
    performance_optimizer.cpp
    pipeline.cpp
    numa_utils.cpp
)

set(HEADERS
    config.h
    compressor.h
    monitor.h
    logger.h
    threadpool.h
    security.h
    memory_pool.h
    async_io.h
    performance_optimizer.h
    pipeline.h
    numa_utils.h
)

# ============================================================================
# Создание исполняемого файла
# ============================================================================
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# ============================================================================
# Подключение заголовочных файлов зависимостей
# ============================================================================
target_include_directories(${PROJECT_NAME} PRIVATE
    ${ZLIB_INCLUDE_DIRS}
    ${BROTLI_INCLUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
)

# Добавляем пути для SELinux если найден
if(SELINUX_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${SELINUX_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_SELINUX)
    message(STATUS "SELinux support enabled")
else()
    message(STATUS "SELinux support disabled (libselinux not found)")
endif()

# Добавляем io_uring если найден
if(LIBURING_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBURING_LIBRARIES})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_IO_URING=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_IO_URING=0)
endif()

# Добавляем NUMA если найден
if(NUMA_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${NUMA_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${NUMA_LIBRARIES})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_NUMA=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_NUMA=0)
endif()

# ============================================================================
# Линковка библиотек
# ============================================================================
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${ZLIB_LIBRARIES}
    ${BROTLI_LIBRARIES}
    ${SYSTEMD_LIBRARIES}
    pthread
)

# Добавляем fmt если используется вместо std::format
if(NOT HAS_STD_FORMAT)
    if(fmt_FOUND)
        # Попытка использовать fmt::fmt (из find_package CONFIG)
        target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt)
    elseif(FMT_FOUND)
        # Используем pkg-config переменные
        target_include_directories(${PROJECT_NAME} PRIVATE ${FMT_INCLUDE_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${FMT_LIBRARIES})
    endif()
endif()

# Добавляем SELinux если найден
if(SELINUX_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SELINUX_LIBRARIES})
endif()

# Добавляем Seccomp если найден
if(SECCOMP_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${SECCOMP_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SECCOMP_LIBRARIES})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_SECCOMP)
    message(STATUS "Seccomp support enabled")
else()
    message(STATUS "Seccomp support disabled (libseccomp not found)")
endif()

# Добавляем Libcap если найден
if(LIBCAP_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBCAP_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBCAP_LIBRARIES})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_LIBCAP)
    message(STATUS "Libcap support enabled")
else()
    message(STATUS "Libcap support disabled (libcap not found)")
endif()

# ============================================================================
# Флаги компиляции
# ============================================================================
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O3
    -march=native
    -flto=auto
    -funroll-loops
)

# Флаги линковки для оптимизации
target_link_options(${PROJECT_NAME} PRIVATE
    -flto=auto
    -Wl,-O1
    -Wl,--as-needed
)

# ============================================================================
# Требования к компилятору (ТЗ п. 1.3)
# ============================================================================
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13.0)
        message(WARNING "GCC версии 13+ рекомендуется для полной поддержки C++23")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.0)
        message(WARNING "Clang версии 16+ рекомендуется для полной поддержки C++23")
    endif()
endif()

# ============================================================================
# Установка файлов (ТЗ п. 10.4)
# ============================================================================
install(TARGETS ${PROJECT_NAME}
    DESTINATION /usr/bin
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/compressor-manager.conf
    DESTINATION /etc/mediahive
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/mh-compressor-manager.service
    DESTINATION /usr/lib/systemd/system
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

# ============================================================================
# Вывод информации о сборке
# ============================================================================
message(STATUS "")
message(STATUS "=== mh-compressor-manager Build Configuration ===")
message(STATUS "CMake Version:        ${CMAKE_VERSION}")
message(STATUS "C++ Standard:         ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:             ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build Type:           ${CMAKE_BUILD_TYPE}")
message(STATUS "ZLIB:                 ${ZLIB_VERSION} (${ZLIB_INCLUDE_DIRS})")
message(STATUS "Brotli:               ${BROTLI_VERSION} (${BROTLI_INCLUDE_DIRS})")
message(STATUS "Systemd:              ${SYSTEMD_VERSION} (${SYSTEMD_INCLUDE_DIRS})")
message(STATUS "SELinux:              ${SELINUX_FOUND}")
message(STATUS "io_uring:             ${HAS_IO_URING}")
message(STATUS "NUMA:                 ${HAS_NUMA}")
message(STATUS "============================================")
message(STATUS "")