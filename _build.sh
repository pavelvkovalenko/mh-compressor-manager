#!/bin/bash
# Сборка mh-compressor-manager с автоматической проверкой и установкой зависимостей

set -e

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== Проверка зависимостей mh-compressor-manager ===${NC}"

# Определение дистрибутива
PKG_MANAGER=""
INSTALL_CMD=""

if command -v dnf &> /dev/null; then
    PKG_MANAGER="dnf"
    INSTALL_CMD="sudo dnf install -y"
elif command -v apt-get &> /dev/null; then
    PKG_MANAGER="apt"
    INSTALL_CMD="sudo apt-get install -y"
elif command -v zypper &> /dev/null; then
    PKG_MANAGER="zypper"
    INSTALL_CMD="sudo zypper install -y"
elif command -v pacman &> /dev/null; then
    PKG_MANAGER="pacman"
    INSTALL_CMD="sudo pacman -S --noconfirm"
else
    echo -e "${RED}Не удалось определить менеджер пакетов. Пожалуйста, установите зависимости вручную.${NC}"
fi

# Массивы для хранения недостающих пакетов
DEPS_TO_INSTALL=()
OPT_DEPS_TO_INSTALL=()

echo -e "${BLUE}Проверка обязательных зависимостей...${NC}"

# ZLIB
if ! pkg-config --exists zlib 2>/dev/null && [ ! -f "/usr/include/z.h" ] && [ ! -f "/usr/local/include/z.h" ]; then
    if [ "$PKG_MANAGER" == "dnf" ]; then DEPS_TO_INSTALL+=("zlib-devel"); 
    elif [ "$PKG_MANAGER" == "apt" ]; then DEPS_TO_INSTALL+=("zlib1g-dev");
    elif [ "$PKG_MANAGER" == "zypper" ]; then DEPS_TO_INSTALL+=("zlib-devel");
    elif [ "$PKG_MANAGER" == "pacman" ]; then DEPS_TO_INSTALL+=("zlib"); fi
fi

# Brotli
if ! pkg-config --exists libbrotlicommon 2>/dev/null && [ ! -f "/usr/include/brotli/decode.h" ] && [ ! -f "/usr/local/include/brotli/decode.h" ]; then
    if [ "$PKG_MANAGER" == "dnf" ]; then DEPS_TO_INSTALL+=("brotli-devel"); 
    elif [ "$PKG_MANAGER" == "apt" ]; then DEPS_TO_INSTALL+=("libbrotli-dev");
    elif [ "$PKG_MANAGER" == "zypper" ]; then DEPS_TO_INSTALL+=("libbrotli-devel");
    elif [ "$PKG_MANAGER" == "pacman" ]; then DEPS_TO_INSTALL+=("brotli"); fi
fi

# Systemd
if ! pkg-config --exists libsystemd 2>/dev/null; then
    if [ "$PKG_MANAGER" == "dnf" ]; then DEPS_TO_INSTALL+=("systemd-devel"); 
    elif [ "$PKG_MANAGER" == "apt" ]; then DEPS_TO_INSTALL+=("libsystemd-dev");
    elif [ "$PKG_MANAGER" == "zypper" ]; then DEPS_TO_INSTALL+=("libsystemd-devel");
    elif [ "$PKG_MANAGER" == "pacman" ]; then DEPS_TO_INSTALL+=("systemd-libs"); fi
fi

# Fmt
if ! pkg-config --exists fmt 2>/dev/null && [ ! -f "/usr/include/fmt/core.h" ] && [ ! -f "/usr/local/include/fmt/core.h" ]; then
    if [ "$PKG_MANAGER" == "dnf" ]; then DEPS_TO_INSTALL+=("fmt-devel"); 
    elif [ "$PKG_MANAGER" == "apt" ]; then DEPS_TO_INSTALL+=("libfmt-dev");
    elif [ "$PKG_MANAGER" == "zypper" ]; then DEPS_TO_INSTALL+=("fmt-devel");
    elif [ "$PKG_MANAGER" == "pacman" ]; then DEPS_TO_INSTALL+=("fmt"); fi
fi

echo -e "${BLUE}Проверка опциональных зависимостей (для производительности)...${NC}"

# Liburing (io_uring)
if ! pkg-config --exists liburing 2>/dev/null; then
    if [ "$PKG_MANAGER" == "dnf" ]; then OPT_DEPS_TO_INSTALL+=("liburing-devel"); 
    elif [ "$PKG_MANAGER" == "apt" ]; then OPT_DEPS_TO_INSTALL+=("liburing-dev");
    elif [ "$PKG_MANAGER" == "zypper" ]; then OPT_DEPS_TO_INSTALL+=("liburing-devel");
    elif [ "$PKG_MANAGER" == "pacman" ]; then OPT_DEPS_TO_INSTALL+=("liburing"); fi
fi

# Numa (NUMA support)
if ! pkg-config --exists numa 2>/dev/null; then
    if [ "$PKG_MANAGER" == "dnf" ]; then OPT_DEPS_TO_INSTALL+=("numactl-devel"); 
    elif [ "$PKG_MANAGER" == "apt" ]; then OPT_DEPS_TO_INSTALL+=("libnuma-dev");
    elif [ "$PKG_MANAGER" == "zypper" ]; then OPT_DEPS_TO_INSTALL+=("numactl-devel");
    elif [ "$PKG_MANAGER" == "pacman" ]; then OPT_DEPS_TO_INSTALL+=("numactl"); fi
fi

# Seccomp
if ! pkg-config --exists libseccomp 2>/dev/null; then
    if [ "$PKG_MANAGER" == "dnf" ]; then OPT_DEPS_TO_INSTALL+=("libseccomp-devel"); 
    elif [ "$PKG_MANAGER" == "apt" ]; then OPT_DEPS_TO_INSTALL+=("libseccomp-dev");
    elif [ "$PKG_MANAGER" == "zypper" ]; then OPT_DEPS_TO_INSTALL+=("libseccomp-devel");
    elif [ "$PKG_MANAGER" == "pacman" ]; then OPT_DEPS_TO_INSTALL+=("libseccomp"); fi
fi

# Libcap
if ! pkg-config --exists libcap 2>/dev/null; then
    if [ "$PKG_MANAGER" == "dnf" ]; then OPT_DEPS_TO_INSTALL+=("libcap-devel"); 
    elif [ "$PKG_MANAGER" == "apt" ]; then OPT_DEPS_TO_INSTALL+=("libcap-dev");
    elif [ "$PKG_MANAGER" == "zypper" ]; then OPT_DEPS_TO_INSTALL+=("libcap-devel");
    elif [ "$PKG_MANAGER" == "pacman" ]; then OPT_DEPS_TO_INSTALL+=("libcap"); fi
fi

NEED_INSTALL=false
INSTALL_OPT=false

# Отчет о_missing зависимостях
if [ ${#DEPS_TO_INSTALL[@]} -ne 0 ]; then
    echo -e "${RED}Отсутствуют обязательные зависимости:${NC}"
    printf '  %s\n' "${DEPS_TO_INSTALL[@]}"
    NEED_INSTALL=true
fi

if [ ${#OPT_DEPS_TO_INSTALL[@]} -ne 0 ]; then
    echo -e "${YELLOW}Отсутствуют опциональные зависимости (рекомендованы для производительности):${NC}"
    printf '  %s\n' "${OPT_DEPS_TO_INSTALL[@]}"
    echo -e "${YELLOW}Без них сборка пройдет, но часть функций будет отключена:${NC}"
    echo -e "${YELLOW}  - liburing: асинхронный I/O (io_uring)${NC}"
    echo -e "${YELLOW}  - numactl: NUMA-aware распределение памяти${NC}"
    echo -e "${YELLOW}  - libseccomp: sandboxing системных вызовов${NC}"
    echo -e "${YELLOW}  - libcap: управление capabilities${NC}"
    echo ""
    read -p "Хотите установить опциональные зависимости? (y/n): " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        INSTALL_OPT=true
        NEED_INSTALL=true
    fi
fi

# Установка зависимостей
if [ "$NEED_INSTALL" = true ]; then
    if [ -z "$PKG_MANAGER" ]; then
        echo -e "${RED}Ошибка: Менеджер пакетов не найден. Установите зависимости вручную.${NC}"
        exit 1
    fi
    
    ALL_DEPS=("${DEPS_TO_INSTALL[@]}")
    if [ "$INSTALL_OPT" = true ]; then
        ALL_DEPS+=("${OPT_DEPS_TO_INSTALL[@]}")
    fi
    
    echo -e "${GREEN}Команда для установки: ${INSTALL_CMD} ${ALL_DEPS[*]}${NC}"
    echo ""
    read -p "Требуется ввод пароля sudo для установки пакетов. Продолжить? (y/n): " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${BLUE}Установка зависимостей...${NC}"
        eval "$INSTALL_CMD ${ALL_DEPS[*]}"
        echo -e "${GREEN}Зависимости установлены успешно!${NC}"
    else
        echo -e "${YELLOW}Установка отменена пользователем.${NC}"
        if [ ${#DEPS_TO_INSTALL[@]} -ne 0 ]; then
            echo -e "${RED}Невозможно продолжить сборку без обязательных зависимостей.${NC}"
            exit 1
        fi
        echo -e "${YELLOW}Продолжение сборки без опциональных зависимостей...${NC}"
    fi
fi

# Сборка проекта
echo -e "${GREEN}=== Начало сборки mh-compressor-manager ===${NC}"
cd "$(dirname "$0")/src"
mkdir -p build
cd build

if [ -f Makefile ]; then
    make clean 2>/dev/null || true
fi

cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo -e "${GREEN}=== Сборка завершена успешно ===${NC}"
echo -e "${BLUE}Бинарный файл: $(pwd)/mh-compressor-manager${NC}"
