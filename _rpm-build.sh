#!/bin/bash
# =============================================================================
# Скрипт сборки RPM-пакета для mh-compressor-manager
# Использование: ./build-rpm.sh [version] [--clean]
# =============================================================================

set -e

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Парсинг аргументов
VERSION=""
CLEAN_OLD=false
AUTO_INSTALL=false
for arg in "$@"; do
    case "$arg" in
        --clean|-c) CLEAN_OLD=true ;;
        --yes|-y) AUTO_INSTALL=true ;;
        --help|-h)
            echo "Использование: $0 [version] [--clean] [--yes]"
            echo "  version    Версия пакета (по умолчанию: из SPEC)"
            echo "  --clean    Очистить старые артефакты сборки перед началом"
            echo "  --yes      Автоматически установить недостающие зависимости"
            exit 0
            ;;
        -*)
            echo -e "${RED}✗ Неизвестный параметр: ${arg}${NC}"
            exit 2
            ;;
        *) VERSION="$arg" ;;
    esac
done

PROJECT_NAME="mh-compressor-manager"
SPEC_FILE="${PROJECT_NAME}.spec"

# Получение версии из SPEC если не указана
if [ -z "$VERSION" ]; then
    if [ -f "$SPEC_FILE" ]; then
        VERSION=$(grep '^Version:' "$SPEC_FILE" | awk '{print $2}')
    fi
    VERSION=${VERSION:-1.0.0}
fi

DISTRO=$(rpm -E %fedora 2>/dev/null || rpm -E %rhel 2>/dev/null || echo "unknown")
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Сборка RPM: ${PROJECT_NAME} v${VERSION}${NC}"
echo -e "${GREEN}  Дистрибутив: Fedora/RHEL ${DISTRO}${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""

# =============================================================================
# [0/5] Очистка старых артефактов (опционально)
# =============================================================================
if [ "$CLEAN_OLD" = true ]; then
    echo -e "${YELLOW}[0/5] Очистка старых артефактов...${NC}"
    RPM_BUILD_ROOT=$(rpm --eval '%{_topdir}' 2>/dev/null || echo "$HOME/rpmbuild")
    if [ -d "${RPM_BUILD_ROOT}" ]; then
        rm -rf "${RPM_BUILD_ROOT}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
        echo -e "${GREEN}✓ Старые артефакты удалены${NC}"
    else
        echo "  Директория ${RPM_BUILD_ROOT} не существует, очистка не требуется"
    fi
    echo ""
fi

# =============================================================================
# [1/5] Проверка зависимостей
# =============================================================================
echo -e "${YELLOW}[1/5] Проверка зависимостей...${NC}"

check_package() {
    local pkg_new=$1
    local pkg_old=$2
    if rpm -q "$pkg_new" &>/dev/null; then
        return 0
    elif [ -n "$pkg_old" ] && rpm -q "$pkg_old" &>/dev/null; then
        return 0
    fi
    return 1
}

declare -A PACKAGES=(
    ["zlib-ng-compat-devel"]="zlib-devel"
    ["brotli-devel"]=""
    ["systemd-devel"]=""
    ["libselinux-devel"]=""
    ["cmake"]=""
    ["gcc-c++"]=""
    ["rpm-build"]=""
    ["pkgconf-pkg-config"]="pkgconfig"
    ["rpmdevtools"]=""
)

MISSING=""
for pkg_new in "${!PACKAGES[@]}"; do
    pkg_old="${PACKAGES[$pkg_new]}"
    if ! check_package "$pkg_new" "$pkg_old"; then
        if [ -n "$pkg_old" ]; then
            MISSING="${MISSING} ${pkg_new}/${pkg_old}"
        else
            MISSING="${MISSING} ${pkg_new}"
        fi
    fi
done

if [ -n "$MISSING" ]; then
    echo -e "${RED}✗ Отсутствуют пакеты:${MISSING}${NC}"
    echo ""

    if [ "$AUTO_INSTALL" = true ]; then
        # Автоматическая установка (флаг --yes)
        echo -e "${YELLOW}Автоматическая установка зависимостей...${NC}"
        sudo dnf install -y zlib-ng-compat-devel brotli-devel systemd-devel libselinux-devel cmake gcc-c++ rpm-build pkgconf-pkg-config rpmdevtools
    else
        # Интерактивный режим — спрашиваем пользователя
        echo -e "${YELLOW}Хотите установить отсутствующие зависимости автоматически? (y/n): ${NC}"
        read -r -n 1 -s REPLY
        echo ""
        if [[ "$REPLY" =~ ^[Yy]$ ]]; then
            echo -e "${BLUE}Установка зависимостей...${NC}"
            sudo dnf install -y zlib-ng-compat-devel brotli-devel systemd-devel libselinux-devel cmake gcc-c++ rpm-build pkgconf-pkg-config rpmdevtools
        else
            echo -e "${RED}Установка отменена. Установите зависимости вручную:${NC}"
            echo "  sudo dnf install zlib-ng-compat-devel brotli-devel systemd-devel libselinux-devel cmake gcc-c++ rpm-build rpmdevtools"
            exit 1
        fi
    fi
    echo -e "${GREEN}✓ Зависимости установлены успешно${NC}"
fi

# Финальная проверка что все зависимости действительно установлены
MISSING_FINAL=""
for pkg_new in "${!PACKAGES[@]}"; do
    pkg_old="${PACKAGES[$pkg_new]}"
    if ! check_package "$pkg_new" "$pkg_old"; then
        if [ -n "$pkg_old" ]; then
            MISSING_FINAL="${MISSING_FINAL} ${pkg_new}/${pkg_old}"
        else
            MISSING_FINAL="${MISSING_FINAL} ${pkg_new}"
        fi
    fi
done

if [ -n "$MISSING_FINAL" ]; then
    echo -e "${RED}✗ Не удалось установить пакеты:${MISSING_FINAL}${NC}"
    echo "Установите вручную: sudo dnf install zlib-ng-compat-devel brotli-devel systemd-devel libselinux-devel cmake gcc-c++ rpm-build rpmdevtools"
    exit 1
fi

echo -e "${GREEN}✓ Все зависимости установлены${NC}"
echo ""

# =============================================================================
# [2/5] Создание структуры директорий RPM
# =============================================================================
echo -e "${YELLOW}[2/5] Создание структуры директорий...${NC}"

RPM_BUILD_ROOT=$(rpm --eval '%{_topdir}')
mkdir -p "${RPM_BUILD_ROOT}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

echo -e "${GREEN}✓ Структура RPM создана в: ${RPM_BUILD_ROOT}${NC}"
echo ""

# =============================================================================
# [3/5] Подготовка исходных кодов
# =============================================================================
echo -e "${YELLOW}[3/5] Подготовка исходных кодов...${NC}"

SOURCE_TAR="${PROJECT_NAME}-${VERSION}.tar.gz"
TEMP_DIR=$(mktemp -d)
BUILD_DIR="${TEMP_DIR}/${PROJECT_NAME}-${VERSION}"

mkdir -p "${BUILD_DIR}"

if [ ! -d "src" ]; then
    echo -e "${RED}✗ Ошибка: Директория 'src' не найдена!${NC}"
    rm -rf "${TEMP_DIR}"
    exit 1
fi

# Копирование исходных файлов из src/
echo "  Копирование исходных файлов..."
cp -r src/* "${BUILD_DIR}/"

# Копирование файлов из корня проекта
echo "  Копирование файлов документации из корня..."
cp "README.md" "${BUILD_DIR}/" 2>/dev/null || echo "  ⚠️ README.md не найден в корне"
cp "LICENSE" "${BUILD_DIR}/" 2>/dev/null || touch "${BUILD_DIR}/LICENSE"

# Копирование дополнительных директорий (man, completion, translations)
echo "  Копирование дополнительных директорий..."
for dir in man completion translations; do
    if [ -d "$dir" ]; then
        cp -r "$dir" "${BUILD_DIR}/"
        echo "    ✓ ${dir}/ скопирована"
    else
        echo "    ⚠️ ${dir}/ не найдена"
    fi
done

# Удаляем артефакты сборки
rm -rf "${BUILD_DIR}/build"
rm -f "${BUILD_DIR}/CMakeCache.txt"
rm -rf "${BUILD_DIR}/CMakeFiles"
rm -f "${BUILD_DIR}/compile_commands.json"
rm -f "${BUILD_DIR}"/*.o
rm -f "${BUILD_DIR}"/*.cmake
rm -f "${BUILD_DIR}/Makefile"

# Генерация compressor-manager.conf
echo "  Копирование дополнительных файлов..."
_generate_config() {
    cat << 'CONF'
[general]
target_path=/var/www/html
debug=false
threads=0
list=txt js css svg json html htm map
algorithms=all
gzip_level=9
brotli_level=11
debounce_delay=2
min_compress_size=256
CONF
}

if [ -f "src/compressor-manager.conf" ]; then
    cp "src/compressor-manager.conf" "${BUILD_DIR}/"
else
    _generate_config > "${BUILD_DIR}/compressor-manager.conf"
fi

# Генерация mh-compressor-manager.service
_generate_service() {
    cat << 'SVC'
[Unit]
Description=MediaHive Compressor Manager
After=network.target

[Service]
Type=notify
ExecStart=/usr/bin/mh-compressor-manager --config /etc/mediahive/compressor-manager.conf
Restart=on-failure
User=root
Group=root

[Install]
WantedBy=multi-user.target
SVC
}

if [ -f "src/mh-compressor-manager.service" ]; then
    cp "src/mh-compressor-manager.service" "${BUILD_DIR}/"
else
    _generate_service > "${BUILD_DIR}/mh-compressor-manager.service"
fi

# Создание архива
echo "  Создание архива: ${SOURCE_TAR}"
tar -czf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}" \
    -C "${TEMP_DIR}" "${PROJECT_NAME}-${VERSION}"

# Проверка структуры архива
echo "  Проверка структуры архива..."
_REQUIRED_FILES=("CMakeLists.txt" "compressor-manager.conf" "mh-compressor-manager.service" "LICENSE")

for req in "${_REQUIRED_FILES[@]}"; do
    if ! tar -tzf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}" | grep -q "${req}"; then
        echo -e "${RED}✗ Ошибка: ${req} не найден в архиве!${NC}"
        echo "Содержимое архива:"
        tar -tzf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}"
        rm -rf "${TEMP_DIR}"
        exit 1
    fi
done

if ! tar -tzf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}" | grep -qE "\.(cpp|h)$"; then
    echo -e "${RED}✗ Ошибка: Исходные файлы (.cpp/.h) не найдены в архиве!${NC}"
    tar -tzf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}"
    rm -rf "${TEMP_DIR}"
    exit 1
fi

rm -rf "${TEMP_DIR}"

echo -e "${GREEN}✓ Исходный архив создан:${NC}"
echo "  ${BLUE}${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}${NC}"
echo ""

# =============================================================================
# [4/5] Обновление и установка SPEC-файла
# =============================================================================
echo -e "${YELLOW}[4/5] Обновление SPEC-файла...${NC}"

if [ ! -f "${SPEC_FILE}" ]; then
    echo -e "${RED}✗ Ошибка: Файл ${SPEC_FILE} не найден!${NC}"
    exit 1
fi

# Сохраняем оригинал SPEC для восстановления
SPEC_BACKUP="${SPEC_FILE}.bak"
cp "${SPEC_FILE}" "${SPEC_BACKUP}"
trap 'mv "${SPEC_BACKUP}" "${SPEC_FILE}" 2>/dev/null || true' EXIT

sed -i "s/^Version:.*/Version:        ${VERSION}/" "${SPEC_FILE}"
sed -i "s/^Release:.*/Release:        1%{?dist}/" "${SPEC_FILE}"

cp "${SPEC_FILE}" "${RPM_BUILD_ROOT}/SPECS/"

echo -e "${GREEN}✓ SPEC-файл обновлён (Version: ${VERSION}) и установлен${NC}"
echo ""

# =============================================================================
# [5/5] Сборка RPM-пакета
# =============================================================================
echo -e "${YELLOW}[5/5] Сборка RPM-пакета...${NC}"
echo ""

rpmbuild -bb "${RPM_BUILD_ROOT}/SPECS/${SPEC_FILE}" \
    --define "_topdir ${RPM_BUILD_ROOT}" \
    --define "dist .fc${DISTRO}"

# Восстановление оригинала SPEC
mv "${SPEC_BACKUP}" "${SPEC_FILE}"

# =============================================================================
# Результат
# =============================================================================
echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  ✓ Сборка завершена успешно!${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""

RPM_FILE=$(find "${RPM_BUILD_ROOT}/RPMS" -name "${PROJECT_NAME}-${VERSION}-1*.rpm" -type f 2>/dev/null | head -1)

if [ -n "${RPM_FILE}" ]; then
    echo -e "${GREEN}RPM-пакет:${NC}"
    echo -e "  ${BLUE}${RPM_FILE}${NC}"
    echo ""
    echo -e "${YELLOW}Установка:${NC}"
    echo -e "  sudo dnf install ${RPM_FILE}"
    echo ""
    echo -e "${YELLOW}Проверка:${NC}"
    echo -e "  rpm -qi ${PROJECT_NAME}"
    echo -e "  systemctl status ${PROJECT_NAME}"
    echo ""
else
    echo -e "${RED}✗ Предупреждение: Файл RPM не найден${NC}"
    exit 1
fi
