cmake_minimum_required(VERSION 3.20)
project(mh-compressor-manager LANGUAGES CXX)

# ============================================================================
# Стандарт C++23 (ТЗ п. 1.2)
# ============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Поиск зависимостей через pkg-config (ТЗ п. 10.2)
# ============================================================================
find_package(PkgConfig REQUIRED)

# zlib - сжатие Gzip
pkg_check_modules(ZLIB REQUIRED zlib)

# Brotli - сжатие Brotli (нужны обе библиотеки: common и enc)
pkg_check_modules(BROTLI REQUIRED libbrotlienc libbrotlicommon)

# systemd - интеграция с systemd (sd_notify)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)

# SELinux - опциональная поддержка контекстов безопасности (ТЗ п. 2.5)
pkg_check_modules(SELINUX libselinux)

# ============================================================================
# Исходные файлы
# ============================================================================
set(SOURCES
    main.cpp
    config.cpp
    compressor.cpp
    monitor.cpp
    logger.cpp
)

set(HEADERS
    config.h
    compressor.h
    monitor.h
    logger.h
    threadpool.h
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

# ============================================================================
# Линковка библиотек
# ============================================================================
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${ZLIB_LIBRARIES}
    ${BROTLI_LIBRARIES}
    ${SYSTEMD_LIBRARIES}
    pthread
)

# Добавляем SELinux если найден
if(SELINUX_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SELINUX_LIBRARIES})
endif()

# ============================================================================
# Флаги компиляции
# ============================================================================
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O2
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
message(STATUS "============================================")
message(STATUS "")