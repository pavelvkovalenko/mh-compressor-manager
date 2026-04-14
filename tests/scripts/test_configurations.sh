#!/bin/bash
# =============================================================================
# test_configurations.sh — Тестирование комбинаций конфигурации (ТЗ §18.4)
# Проверяет CFG-1 через CFG-6
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/config_test"
SERVICE="mh-compressor-manager"
PASS=0
FAIL=0
TOTAL=0
TMP_CONF="/tmp/compressor_test_conf.conf"

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }
check() {
    TOTAL=$((TOTAL + 1))
    if bash -c "$1" 2>/dev/null; then
        PASS=$((PASS + 1))
        echo "  ✅ PASS [$TOTAL]: $2"
    else
        FAIL=$((FAIL + 1))
        echo "  ❌ FAIL [$TOTAL]: $2"
    fi
}

cleanup() {
    sudo systemctl stop "$SERVICE" 2>/dev/null || true
    # Восстановить оригинальный конфиг из бэкапа
    if [[ -f /etc/mediahive/compressor-manager.conf.bak ]]; then
        sudo cp /etc/mediahive/compressor-manager.conf.bak /etc/mediahive/compressor-manager.conf
    fi
    sudo rm -rf "$BASEDIR" "$TMP_CONF"
}
trap cleanup EXIT

# Создание временного конфигурационного файла
create_config() {
    local algorithms="$1"
    local gzip_level="${2:-9}"
    local brotli_level="${3:-11}"
    cat > "$TMP_CONF" << EOF
[general]
target_path=$BASEDIR
debug=true
threads=2
list=txt js json html htm css
algorithms=$algorithms
gzip_level=$gzip_level
brotli_level=$brotli_level
debounce_delay=2
min_compress_size=256
EOF
}

# Тестовые файлы
create_test_files() {
    sudo rm -rf "$BASEDIR"
    sudo mkdir -p "$BASEDIR"
    python3 -c "print('X' * 500)" > "$BASEDIR/test_file.js"
    python3 -c "print('Y' * 100)" > "$BASEDIR/small_file.js"
}

# Запуск сервиса с конфигурацией и ожидание обработки
run_with_config() {
    sudo systemctl stop "$SERVICE" 2>/dev/null || true
    sleep 1
    # Копируем конфиг на место
    sudo cp "$TMP_CONF" /etc/mediahive/compressor-manager.conf
    sudo systemctl daemon-reload
    sudo systemctl start "$SERVICE"
    sleep 10
    sudo systemctl stop "$SERVICE" 2>/dev/null || true
    sleep 2
}

# ===========================================================================
log "=== Подготовка ==="
cleanup
sudo mkdir -p "$BASEDIR"

# ===========================================================================
log "=== CFG-1: Только gzip, level 9 ==="
create_config "gzip" 9
create_test_files
run_with_config

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-1a: .gz создана (gzip only)"
check "[ ! -f '$BASEDIR/test_file.js.br' ]" "CFG-1b: .br НЕ создана (gzip only)"
check "[ ! -f '$BASEDIR/small_file.js.gz' ]" "CFG-1c: файл < 256 байт не сжат"

# ===========================================================================
log "=== CFG-2: Только brotli, level 11 ==="
create_config "brotli" 9 11
create_test_files
run_with_config

check "[ ! -f '$BASEDIR/test_file.js.gz' ]" "CFG-2a: .gz НЕ создана (brotli only)"
check "[ -f '$BASEDIR/test_file.js.br' ]" "CFG-2b: .br создана (brotli only)"

# ===========================================================================
log "=== CFG-3: Оба алгоритма, max сжатие ==="
create_config "all" 9 11
create_test_files
run_with_config

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-3a: .gz создана"
check "[ -f '$BASEDIR/test_file.js.br' ]" "CFG-3b: .br создана"

# Валидация
if [ -f "$BASEDIR/test_file.js.gz" ]; then
    TMP=$(mktemp)
    gunzip -c "$BASEDIR/test_file.js.gz" > "$TMP" 2>/dev/null
    ORIG_HASH=$(md5sum "$BASEDIR/test_file.js" | cut -d' ' -f1)
    DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
    check "[ '$ORIG_HASH' = '$DECOMP_HASH' ]" "CFG-3c: .gz валидна"
    rm -f "$TMP"
fi

if [ -f "$BASEDIR/test_file.js.br" ]; then
    TMP=$(mktemp)
    brotli -d -c "$BASEDIR/test_file.js.br" > "$TMP" 2>/dev/null
    ORIG_HASH=$(md5sum "$BASEDIR/test_file.js" | cut -d' ' -f1)
    DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
    check "[ '$ORIG_HASH' = '$DECOMP_HASH' ]" "CFG-3d: .br валидна"
    rm -f "$TMP"
fi

# ===========================================================================
log "=== CFG-4: Оба алгоритма, среднее сжатие ==="
create_config "all" 6 6
create_test_files
run_with_config

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-4a: .gz создана (level 6)"
check "[ -f '$BASEDIR/test_file.js.br' ]" "CFG-4b: .br создана (level 6)"

# ===========================================================================
log "=== CFG-5: Оба алгоритма, min сжатие ==="
create_config "all" 1 1
create_test_files
run_with_config

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-5a: .gz создана (level 1)"
check "[ -f '$BASEDIR/test_file.js.br' ]" "CFG-5b: .br создана (level 1)"

# ===========================================================================
log "=== CFG-6: Только gzip, level 6 (по умолчанию) ==="
create_config "gzip" 6
create_test_files
run_with_config

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-6a: .gz создана (level 6)"
check "[ ! -f '$BASEDIR/test_file.js.br' ]" "CFG-6b: .br НЕ создана (gzip only)"

# ===========================================================================
log "=== Идемпотентность (повторный запуск не пережимает актуальные файлы) ==="
create_config "all" 9 11
create_test_files
run_with_config

GZ_MTIME1=$(stat -c %Y "$BASEDIR/test_file.js.gz" 2>/dev/null || echo 0)
BR_MTIME1=$(stat -c %Y "$BASEDIR/test_file.js.br" 2>/dev/null || echo 0)

# Повторный запуск с той же конфигурацией
run_with_config

GZ_MTIME2=$(stat -c %Y "$BASEDIR/test_file.js.gz" 2>/dev/null || echo 0)
BR_MTIME2=$(stat -c %Y "$BASEDIR/test_file.js.br" 2>/dev/null || echo 0)

check "[ $GZ_MTIME1 -eq $GZ_MTIME2 ]" "IDEMP-1: .gz mtime не изменился при повторном запуске"
check "[ $BR_MTIME1 -eq $BR_MTIME2 ]" "IDEMP-2: .br mtime не изменился при повторном запуске"

# ===========================================================================
log "=== Удаление оригинала — сжатые копии удаляются ==="
sudo rm -f "$BASEDIR/test_file.js"
sleep 5

check "[ ! -f '$BASEDIR/test_file.js.gz' ]" "DELETE-1: .gz удалена при удалении оригинала"
check "[ ! -f '$BASEDIR/test_file.js.br' ]" "DELETE-2: .br удалена при удалении оригинала"

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ CONFIGURATIONS: $PASS/$TOTAL пройдено, $FAIL провалено ==="

# Восстановление оригинального конфига
sudo cp /etc/mediahive/compressor-manager.conf.bak /etc/mediahive/compressor-manager.conf 2>/dev/null || true
sudo systemctl daemon-reload

exit $FAIL
