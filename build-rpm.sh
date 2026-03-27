#!/bin/bash
# =============================================================================
# Скрипт сборки RPM-пакета для mh-compressor-manager
# Использование: ./build-rpm.sh [version]
# =============================================================================

set -e

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

VERSION=${1:-1.0.0}
PROJECT_NAME="mh-compressor-manager"
SPEC_FILE="${PROJECT_NAME}.spec"

DISTRO=$(rpm -E %fedora 2>/dev/null || rpm -E %rhel 2>/dev/null || echo "unknown")
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Сборка RPM: ${PROJECT_NAME} v${VERSION}${NC}"
echo -e "${GREEN}  Дистрибутив: Fedora/RHEL ${DISTRO}${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""

# =============================================================================
# [1/6] Проверка зависимостей
# =============================================================================
echo -e "${YELLOW}[1/6] Проверка зависимостей...${NC}"

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
    echo "Установите: sudo dnf install zlib-ng-compat-devel brotli-devel systemd-devel libselinux-devel cmake gcc-c++ rpm-build pkgconf-pkg-config rpmdevtools"
    exit 1
fi

echo -e "${GREEN}✓ Все зависимости установлены${NC}"
echo ""

# =============================================================================
# [2/6] Создание структуры директорий RPM
# =============================================================================
echo -e "${YELLOW}[2/6] Создание структуры директорий...${NC}"

RPM_BUILD_ROOT=$(rpm --eval '%{_topdir}')
mkdir -p "${RPM_BUILD_ROOT}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

echo -e "${GREEN}✓ Структура RPM создана в: ${RPM_BUILD_ROOT}${NC}"
echo ""

# =============================================================================
# [3/6] Подготовка исходных кодов (ИСПРАВЛЕНО - простое копирование)
# =============================================================================
echo -e "${YELLOW}[3/6] Подготовка исходных кодов...${NC}"

SOURCE_TAR="${PROJECT_NAME}-${VERSION}.tar.gz"
TEMP_DIR=$(mktemp -d)
BUILD_DIR="${TEMP_DIR}/${PROJECT_NAME}-${VERSION}"

mkdir -p "${BUILD_DIR}"

if [ ! -d "src" ]; then
    echo -e "${RED}✗ Ошибка: Директория 'src' не найдена!${NC}"
    rm -rf "${TEMP_DIR}"
    exit 1
fi

# Простое копирование всех файлов из src/ в BUILD_DIR
echo "  Копирование исходных файлов..."
cp -r src/* "${BUILD_DIR}/"

# Удаляем артефакты сборки, если они попали
rm -rf "${BUILD_DIR}/build"
rm -f "${BUILD_DIR}/CMakeCache.txt"
rm -rf "${BUILD_DIR}/CMakeFiles"
rm -f "${BUILD_DIR}/compile_commands.json"
rm -f "${BUILD_DIR}"/*.o
rm -f "${BUILD_DIR}"/*.cmake
rm -f "${BUILD_DIR}/Makefile"

# Копирование дополнительных файлов в корень сборки
echo "  Копирование дополнительных файлов..."

# compressor-manager.conf
if [ -f "src/compressor-manager.conf" ]; then
    cp "src/compressor-manager.conf" "${BUILD_DIR}/"
else
    cat > "${BUILD_DIR}/compressor-manager.conf" << 'CONF'
[general]
target_path=/var/www/html
debug=false
threads=0
list=txt js css svg json html htm map
algorithms=all
gzip_level=6
brotli_level=4
debounce_delay=2
CONF
fi

# mh-compressor-manager.service
if [ -f "src/mh-compressor-manager.service" ]; then
    cp "src/mh-compressor-manager.service" "${BUILD_DIR}/"
else
    cat > "${BUILD_DIR}/mh-compressor-manager.service" << 'SVC'
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
fi

# Документация
cp "src/README.md" "${BUILD_DIR}/" 2>/dev/null || touch "${BUILD_DIR}/README.md"
cp "src/README.html" "${BUILD_DIR}/" 2>/dev/null || touch "${BUILD_DIR}/README.html"
cp "LICENSE" "${BUILD_DIR}/" 2>/dev/null || touch "${BUILD_DIR}/LICENSE"
cp "${SPEC_FILE}" "${BUILD_DIR}/" 2>/dev/null || true

# Создание архива
echo "  Создание архива: ${SOURCE_TAR}"
tar -czf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}" \
    -C "${TEMP_DIR}" "${PROJECT_NAME}-${VERSION}"

# Проверка
echo "  Проверка структуры архива..."
if ! tar -tzf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}" | grep -q "CMakeLists.txt"; then
    echo -e "${RED}✗ Ошибка: CMakeLists.txt не найден в архиве!${NC}"
    echo "Содержимое архива:"
    tar -tzf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}"
    rm -rf "${TEMP_DIR}"
    exit 1
fi

if ! tar -tzf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}" | grep -q "\.cpp"; then
    echo -e "${RED}✗ Ошибка: Исходные файлы .cpp не найдены в архиве!${NC}"
    echo "Содержимое архива:"
    tar -tzf "${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}"
    rm -rf "${TEMP_DIR}"
    exit 1
fi

rm -rf "${TEMP_DIR}"

echo -e "${GREEN}✓ Исходный архив создан:${NC}"
echo "  ${BLUE}${RPM_BUILD_ROOT}/SOURCES/${SOURCE_TAR}${NC}"
echo ""

# =============================================================================
# [4/6] Копирование дополнительных файлов в SOURCES
# =============================================================================
echo -e "${YELLOW}[4/6] Подготовка дополнительных файлов...${NC}"

cp "${BUILD_DIR}/compressor-manager.conf" "${RPM_BUILD_ROOT}/SOURCES/" 2>/dev/null || true
cp "${BUILD_DIR}/mh-compressor-manager.service" "${RPM_BUILD_ROOT}/SOURCES/" 2>/dev/null || true
cp "${BUILD_DIR}/README.md" "${RPM_BUILD_ROOT}/SOURCES/" 2>/dev/null || true
cp "${BUILD_DIR}/README.html" "${RPM_BUILD_ROOT}/SOURCES/" 2>/dev/null || true
cp "${BUILD_DIR}/LICENSE" "${RPM_BUILD_ROOT}/SOURCES/" 2>/dev/null || true

echo -e "${GREEN}✓ Дополнительные файлы подготовлены${NC}"
echo ""

# =============================================================================
# [5/6] Обновление и установка SPEC-файла
# =============================================================================
echo -e "${YELLOW}[5/6] Обновление SPEC-файла...${NC}"

if [ -f "${SPEC_FILE}" ]; then
    sed -i "s/^Version:.*/Version:        ${VERSION}/" "${SPEC_FILE}"
    sed -i "s/^Release:.*/Release:        1%{?dist}/" "${SPEC_FILE}"
else
    echo -e "${RED}✗ Ошибка: Файл ${SPEC_FILE} не найден!${NC}"
    exit 1
fi

cp "${SPEC_FILE}" "${RPM_BUILD_ROOT}/SPECS/"

echo -e "${GREEN}✓ SPEC-файл установлен:${NC}"
echo "  ${BLUE}${RPM_BUILD_ROOT}/SPECS/${SPEC_FILE}${NC}"
echo ""

# =============================================================================
# [6/6] Сборка RPM-пакета
# =============================================================================
echo -e "${YELLOW}[6/6] Сборка RPM-пакета...${NC}"
echo ""

rpmbuild -bb "${RPM_BUILD_ROOT}/SPECS/${SPEC_FILE}" \
    --define "_topdir ${RPM_BUILD_ROOT}" \
    --define "dist .fc${DISTRO}"

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
    echo -e "RPM-пакет:${NC}"
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