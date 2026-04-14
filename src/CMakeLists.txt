cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Управление версиями: MAJOR.MINOR.PR_COUNT
# MINOR (после точки) = количество закрытых PR
# Текущее значение обновляется вручную или через CI/CD
# ============================================================================
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 443)  # Количество закрытых PR (обновлять при мерже)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

project(mh-compressor-manager
    VERSION ${PROJECT_VERSION}
    LANGUAGES CXX
)

# Генерируем version.h из шаблона
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
    @ONLY
)

# ============================================================================
# Стандарт C++23 (ТЗ п. 1.2)
# Пробуем C++23 со std::format, откатываемся на C++20 + fmt если недоступен
# ============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_source_compiles("
    #include <format>
    #include <string>
    int main() {
        auto s = std::format(\"{}\", \"test\");
        return 0;
    }
" HAS_STD_FORMAT)
unset(CMAKE_REQUIRED_FLAGS)

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
    message(STATUS "std::format available, using C++23")
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Поиск зависимостей через pkg-config (ТЗ п. 10.2)
# ============================================================================
find_package(PkgConfig REQUIRED)

# zlib - сжатие Gzip (fallback если libdeflate недоступна)
pkg_check_modules(ZLIB QUIET zlib)

# libdeflate — современная замена zlib, 1.5-2x быстрее при том же ratio
pkg_check_modules(LIBDEFLATE QUIET libdeflate)
if(LIBDEFLATE_FOUND)
    message(STATUS "libdeflate found — using for gzip (1.5-2x faster than zlib)")
    add_definitions(-DHAVE_LIBDEFLATE)
elseif(ZLIB_FOUND)
    message(STATUS "libdeflate not found — using zlib (slower)")
else()
    message(FATAL_ERROR
        "Neither libdeflate nor zlib found!\n"
        "Please install libdeflate (preferred) or zlib:\n"
        "  - Fedora: sudo dnf install libdeflate-devel\n"
        "  - Debian/Ubuntu: sudo apt-get install libdeflate-dev\n"
        "  - Fallback: sudo dnf install zlib-devel"
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
    numa_utils.cpp
    cache_info.cpp
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
    numa_utils.h
    cache_info.h
)

# ============================================================================
# Создание исполняемого файла
# ============================================================================
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# ============================================================================
# Gettext — локализация (i18n, ТЗ §22)
# ============================================================================
find_package(Gettext QUIET)
if(Gettext_FOUND AND GETTEXT_MSGFMT_EXECUTABLE)
    message(STATUS "Gettext found — localization enabled")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_GETTEXT)
    # На Linux/glibc libintl встроена в libc, отдельная линковка не нужна.
    # На macOS/MinGW требуется отдельная libintl.
    if(APPLE OR WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE intl)
    endif()
else()
    message(STATUS "Gettext NOT found — localization disabled, English only")
    # Fallback: макрос _() возвращает строку как есть (определено в i18n.h)
endif()

# ============================================================================
# Подключение заголовочных файлов зависимостей
# ============================================================================
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}  # Для сгенерированного version.h
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
# zlib всегда нужен — старый compress_gzip() использует deflate*
# libdeflate — опционально, для ускорения gzip из памяти
if(LIBDEFLATE_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${LIBDEFLATE_LIBRARIES}
        -lz                # zlib всегда нужен для старого compress_gzip()
        ${BROTLI_LIBRARIES}
        ${SYSTEMD_LIBRARIES}
        pthread
    )
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBDEFLATE_INCLUDE_DIRS})
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        -lz                # zlib
        ${BROTLI_LIBRARIES}
        ${SYSTEMD_LIBRARIES}
        pthread
    )
endif()

# Макросы для pipeline.cpp — zlib и brotli всегда доступны
target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_ZLIB HAVE_LIBBROTLI)

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

# Service file
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/mh-compressor-manager.service
    DESTINATION /usr/lib/systemd/system
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

# ============================================================================
# Man-страницы (ТЗ §6.2)
# ============================================================================
# Английская man-страница (оригинал)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../man/en/man1/mh-compressor-manager.1
    DESTINATION /usr/share/man/man1
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    RENAME mh-compressor-manager.1
)

# Русская man-страница (перевод)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../man/ru/man1/mh-compressor-manager.1
    DESTINATION /usr/share/man/ru/man1
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    RENAME mh-compressor-manager.1
)

# ============================================================================
# Вывод информации о сборке
# ============================================================================
# Определяем библиотеку форматирования
if(HAS_STD_FORMAT)
    set(FMT_STATUS "std::format (C++23)")
else()
    set(FMT_STATUS "fmt library (${fmt_VERSION})")
endif()

# Получаем полную версию Brotli
if(BROTLI_FOUND AND NOT BROTLI_VERSION)
    # pkg_check_modules может не вернуть версию, получим её из заголовков
    set(BROTLI_VERSION "(found)")
endif()

message(STATUS "")
message(STATUS "=== mh-compressor-manager Build Configuration ===")
message(STATUS "Version:              ${PROJECT_VERSION}")
message(STATUS "CMake Version:        ${CMAKE_VERSION}")
message(STATUS "C++ Standard:         ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:             ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Compiler Path:        ${CMAKE_CXX_COMPILER}")
message(STATUS "String Formatting:    ${FMT_STATUS}")
message(STATUS "Build Type:           ${CMAKE_BUILD_TYPE}")
message(STATUS "")
message(STATUS "Libraries:")
message(STATUS "  ZLIB:               ${ZLIB_VERSION} (${ZLIB_INCLUDE_DIRS})")
message(STATUS "  Brotli:             ${BROTLI_VERSION} (${BROTLI_INCLUDE_DIRS})")
message(STATUS "  systemd:            ${SYSTEMD_VERSION} (${SYSTEMD_INCLUDE_DIRS})")
message(STATUS "")
message(STATUS "Optional Features:")
if(SELINUX_FOUND)
    message(STATUS "  SELinux:            Yes")
else()
    message(STATUS "  SELinux:            No")
endif()
if(HAS_IO_URING)
    message(STATUS "  io_uring:           Yes")
else()
    message(STATUS "  io_uring:           No")
endif()
if(HAS_NUMA)
    message(STATUS "  NUMA:               Yes")
else()
    message(STATUS "  NUMA:               No")
endif()
message(STATUS "============================================")
message(STATUS "")

# ============================================================================
# Подключение переводов (translations/)
# ============================================================================
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../translations ${CMAKE_CURRENT_BINARY_DIR}/translations)

# ============================================================================
# Подключение тестов (опционально, через отдельную сборку)
# ============================================================================
# Для сборки тестов: cd tests && cmake . -B build && cmake --build build
# enable_testing()
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)