#!/bin/bash
# =============================================================================
# Скрипт управления переводами mh-compressor-manager
# Использование: ./_translate.sh [command] [language]
# =============================================================================
set -euo pipefail

# Цвета
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Пути
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRANSLATIONS_DIR="${SCRIPT_DIR}/translations"
SRC_DIR="${SCRIPT_DIR}/src"
BUILD_DIR="${SCRIPT_DIR}/src/build"
POT_FILE="${TRANSLATIONS_DIR}/mh-compressor-manager.pot"

# Утилиты
log_info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# Проверка наличия gettext
check_gettext() {
    local missing=()
    for cmd in xgettext msgmerge msgfmt msgattrib; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    if [ ${#missing[@]} -gt 0 ]; then
        log_error "Отсутствуют утилиты gettext: ${missing[*]}"
        echo "Установите: sudo dnf install gettext"
        exit 1
    fi
}

# 1. Генерация/обновление шаблона .pot
cmd_pot() {
    log_info "Генерация шаблона перевода ${POT_FILE}..."
    if [ -f "$POT_FILE" ]; then
        log_info "Файл уже существует, обновляем..."
    fi
    mkdir -p "$TRANSLATIONS_DIR"
    xgettext \
        --from-code=UTF-8 \
        --keyword=_ --keyword=N_ \
        --output="$POT_FILE" \
        --package-name="mh-compressor-manager" \
        --package-version="1.0.88" \
        --copyright-holder="MediaHive.ru" \
        ${SRC_DIR}/*.cpp ${SRC_DIR}/*.h
    local count
    count=$(grep -c '^msgid "' "$POT_FILE" || echo 0)
    log_ok "Шаблон обновлён: $count строк для перевода"
}

# 2. Обновление .po из .pot
cmd_update() {
    local lang="${1:-}"
    if [ -z "$lang" ]; then
        echo -e "${YELLOW}Доступные языки:${NC}"
        for f in "${TRANSLATIONS_DIR}"/*.po; do
            [ -f "$f" ] && echo "  $(basename "$f" .po)"
        done
        read -rp "Введите язык (например, ru, de, fr): " lang
    fi
    local po_file="${TRANSLATIONS_DIR}/${lang}.po"
    if [ ! -f "$POT_FILE" ]; then
        log_error "Файл шаблона $POT_FILE не найден. Сначала выполните: $0 pot"
        exit 1
    fi
    if [ -f "$po_file" ]; then
        log_info "Обновление ${lang}.po из шаблона..."
        msgmerge --update --no-fuzzy-matching "$po_file" "$POT_FILE"
        # Подсчёт untranslated
        local untranslated
        untranslated=$(grep -c '^msgid "' "$po_file" | head -1)
        local translated
        translated=$(grep -c '^msgstr "[^"]' "$po_file" || echo 0)
        local empty
        empty=$((untranslated - translated))
        log_ok "Обновлено: ${translated} переведено, ${empty} осталось перевести"
    else
        log_info "Создание нового файла ${lang}.po..."
        cp "$POT_FILE" "$po_file"
        log_warn "Отредактируйте $po_file — заполните msgstr для каждой строки"
    fi
}

# 3. Очистка obsolete строк из .po
cmd_clean() {
    local lang="${1:-}"
    if [ -z "$lang" ]; then
        for f in "${TRANSLATIONS_DIR}"/*.po; do
            [ -f "$f" ] && lang="$(basename "$f" .po)" && break
        done
    fi
    local po_file="${TRANSLATIONS_DIR}/${lang}.po"
    if [ ! -f "$po_file" ]; then
        log_error "Файл $po_file не найден"
        exit 1
    fi
    log_info "Очистка obsolete строк из ${lang}.po..."
    msgattrib --no-obsolete -o "$po_file" "$po_file"
    log_ok "Obsolete строки удалены"
}

# 4. Компиляция .po → .mo
cmd_compile() {
    local lang="${1:-}"
    if [ -z "$lang" ]; then
        for f in "${TRANSLATIONS_DIR}"/*.po; do
            [ -f "$f" ] && lang="$(basename "$f" .po)" && break
        done
    fi
    local po_file="${TRANSLATIONS_DIR}/${lang}.po"
    local mo_file="${TRANSLATIONS_DIR}/${lang}.mo"
    if [ ! -f "$po_file" ]; then
        log_error "Файл $po_file не найден"
        exit 1
    fi
    log_info "Компиляция ${lang}.po → ${lang}.mo..."
    mkdir -p "$(dirname "$mo_file")"
    msgfmt -o "$mo_file" "$po_file"
    log_ok "Скомпилировано: $mo_file"
}

# 5. Установка .mo в систему
cmd_install() {
    local lang="${1:-}"
    if [ -z "$lang" ]; then
        for f in "${TRANSLATIONS_DIR}"/*.mo; do
            [ -f "$f" ] && lang="$(basename "$f" .mo)" && break
        done
    fi
    local mo_file="${TRANSLATIONS_DIR}/${lang}.mo"
    if [ ! -f "$mo_file" ]; then
        log_error "Файл $mo_file не найден. Сначала выполните: $0 compile $lang"
        exit 1
    fi
    log_info "Установка ${lang}.mo в систему..."
    local dest="/usr/share/locale/${lang}/LC_MESSAGES"
    sudo mkdir -p "$dest"
    sudo cp "$mo_file" "${dest}/mh-compressor-manager.mo"
    sudo chmod 644 "${dest}/mh-compressor-manager.mo"
    log_ok "Установлено: ${dest}/mh-compressor-manager.mo"
}

# 6. Тестирование перевода
cmd_test() {
    local lang="${1:-ru}"
    log_info "Тестирование перевода (${lang})..."
    log_info "Запуск: LANG=${lang}_UTF-8 ./mh-compressor-manager --help"
    if [ -x "${BUILD_DIR}/mh-compressor-manager" ]; then
        LANG="${lang}_UTF-8" "${BUILD_DIR}/mh-compressor-manager" --help 2>&1 | head -20
    else
        log_warn "Бинарник не найден. Соберите проект: cd src/build && make"
    fi
}

# 7. Полный цикл: pot → update → compile → install
cmd_all() {
    local lang="${1:-ru}"
    cmd_pot
    cmd_update "$lang"
    cmd_compile "$lang"
    cmd_install "$lang"
    cmd_test "$lang"
}

# Интерактивный режим
cmd_interactive() {
    echo -e "${BLUE}=== Управление переводами mh-compressor-manager ===${NC}"
    echo ""
    echo "1) Обновить шаблон (.pot)"
    echo "2) Обновить перевод (.po из .pot)"
    echo "3) Очистить obsolete строки"
    echo "4) Скомпилировать (.po → .mo)"
    echo "5) Установить .mo в систему"
    echo "6) Протестировать перевод"
    echo "7) Полный цикл (pot → update → compile → install → test)"
    echo ""
    read -rp "Выберите действие (1-7): " choice
    case "$choice" in
        1) cmd_pot ;;
        2) cmd_update ;;
        3) cmd_clean ;;
        4) cmd_compile ;;
        5) cmd_install ;;
        6) cmd_test ;;
        7) cmd_all ;;
        *) log_error "Неверный выбор" ; exit 1 ;;
    esac
}

# ===== Main =====
check_gettext

case "${1:-}" in
    pot)          cmd_pot ;;
    update)       cmd_update "${2:-}" ;;
    clean)        cmd_clean "${2:-}" ;;
    compile)      cmd_compile "${2:-}" ;;
    install)      cmd_install "${2:-}" ;;
    test)         cmd_test "${2:-}" ;;
    all)          cmd_all "${2:-}" ;;
    ""|help|-h)   cmd_interactive ;;
    *)            log_error "Неизвестная команда: $1. Используйте: $0 help" ; exit 1 ;;
esac
