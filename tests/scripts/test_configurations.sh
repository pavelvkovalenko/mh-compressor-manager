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

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }
check() {
    TOTAL=$((TOTAL + 1))
    if eval "$1"; then
        PASS=$((PASS + 1))
        echo "  ✅ PASS [$TOTAL]: $2"
    else
        FAIL=$((FAIL + 1))
        echo "  ❌ FAIL [$TOTAL]: $2"
    fi
}

cleanup() {
    sudo systemctl stop "$SERVICE" 2>/dev/null || true
    sudo rm -rf "$BASEDIR"
}
trap cleanup EXIT

# ===========================================================================
log "=== Подготовка ==="
cleanup
sudo mkdir -p "$BASEDIR"

# Тестовый файл
create_test_file() {
    python3 -c "print('X' * 500)" > "$BASEDIR/test_file.js"
    python3 -c "print('Y' * 100)" > "$BASEDIR/small_file.js"
}

# ===========================================================================
log "=== CFG-1: Только gzip, level 9 ==="
sudo systemctl start "$SERVICE" -- --algorithms=gzip --gzip-level=9 --dir="$BASEDIR" 2>/dev/null || \
    sudo mh-compressor-manager --algorithms=gzip --gzip-level=9 --dir="$BASEDIR" --dry-run &
PID=$!
sleep 1
sudo kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

# Тест через прямой запуск
create_test_file
sudo mh-compressor-manager --algorithms=gzip --gzip-level=9 --dir="$BASEDIR" --debug &
PID=$!
sleep 10
sudo kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-1a: .gz создана (gzip only)"
check "[ ! -f '$BASEDIR/test_file.js.br' ]" "CFG-1b: .br НЕ создана (gzip only)"
check "[ ! -f '$BASEDIR/small_file.js.gz' ]" "CFG-1c: файл < 256 байт не сжат"
rm -f "$BASEDIR"/*.gz "$BASEDIR"/*.br

# ===========================================================================
log "=== CFG-2: Только brotli, level 11 ==="
create_test_file
sudo mh-compressor-manager --algorithms=brotli --brotli-level=11 --dir="$BASEDIR" --debug &
PID=$!
sleep 10
sudo kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

check "[ ! -f '$BASEDIR/test_file.js.gz' ]" "CFG-2a: .gz НЕ создана (brotli only)"
check "[ -f '$BASEDIR/test_file.js.br' ]" "CFG-2b: .br создана (brotli only)"
rm -f "$BASEDIR"/*.gz "$BASEDIR"/*.br

# ===========================================================================
log "=== CFG-3: Оба алгоритма, max сжатие ==="
create_test_file
sudo mh-compressor-manager --algorithms=all --gzip-level=9 --brotli-level=11 --dir="$BASEDIR" --debug &
PID=$!
sleep 10
sudo kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-3a: .gz создана"
check "[ -f '$BASEDIR/test_file.js.br' ]" "CFG-3b: .br создана"

# Валидация
if [ -f "$BASEDIR/test_file.js.gz" ]; then
    TMP=$(mktemp)
    gunzip -c "$BASEDIR/test_file.js.gz" > "$TMP" 2>/dev/null
    ORIG_HASH=$(md5sum "$BASEDIR/test_file.js" | cut -d' ' -f1)
    DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
    check "[ '$ORIG_HASH' = '$DECOMP_HASH' ]" "CFG-3c: .gz валидна"
    GZ_SIZE=$(stat -c %s "$BASEDIR/test_file.js.gz")
    rm -f "$TMP"
fi

if [ -f "$BASEDIR/test_file.js.br" ]; then
    TMP=$(mktemp)
    brotli -d -c "$BASEDIR/test_file.js.br" > "$TMP" 2>/dev/null
    ORIG_HASH=$(md5sum "$BASEDIR/test_file.js" | cut -d' ' -f1)
    DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
    check "[ '$ORIG_HASH' = '$DECOMP_HASH' ]" "CFG-3d: .br валидна"
    BR_SIZE=$(stat -c %s "$BASEDIR/test_file.js.br")
    rm -f "$TMP"
fi
rm -f "$BASEDIR"/*.gz "$BASEDIR"/*.br

# ===========================================================================
log "=== CFG-5: Оба алгоритма, min сжатие ==="
create_test_file
sudo mh-compressor-manager --algorithms=all --gzip-level=1 --brotli-level=1 --dir="$BASEDIR" --debug &
PID=$!
sleep 10
sudo kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-5a: .gz создана (level 1)"
check "[ -f '$BASEDIR/test_file.js.br' ]" "CFG-5b: .br создана (level 1)"

# Проверяем что файлы с минимальным сжатием БОЛЬШЕ чем с максимальным
# (это ожидаемое поведение — level 1 сжимает хуже)
if [ -f "$BASEDIR/test_file.js.gz" ] && [ -n "${GZ_SIZE:-}" ]; then
    GZ_SIZE_L1=$(stat -c %s "$BASEDIR/test_file.js.gz")
    check "[ $GZ_SIZE_L1 -ge ${GZ_SIZE:-0} ]" "CFG-5c: .gz level 1 >= .gz level 9 по размеру"
fi
rm -f "$BASEDIR"/*.gz "$BASEDIR"/*.br

# ===========================================================================
log "=== CFG-6: Только gzip, level 6 (стандартная конфигурация) ==="
create_test_file
sudo mh-compressor-manager --algorithms=gzip --gzip-level=6 --dir="$BASEDIR" --debug &
PID=$!
sleep 10
sudo kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

check "[ -f '$BASEDIR/test_file.js.gz' ]" "CFG-6a: .gz создана (level 6)"
check "[ ! -f '$BASEDIR/test_file.js.br' ]" "CFG-6b: .br НЕ создана (gzip only)"
rm -f "$BASEDIR"/*.gz "$BASEDIR"/*.br

# ===========================================================================
log "=== Идемпотентность (повторный запуск не пережимает актуальные файлы) ==="
create_test_file
sudo mh-compressor-manager --algorithms=all --dir="$BASEDIR" --debug &
PID=$!
sleep 10
sudo kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

GZ_MTIME1=$(stat -c %Y "$BASEDIR/test_file.js.gz" 2>/dev/null || echo 0)
BR_MTIME1=$(stat -c %Y "$BASEDIR/test_file.js.br" 2>/dev/null || echo 0)

# Повторный запуск
sudo mh-compressor-manager --algorithms=all --dir="$BASEDIR" --debug &
PID=$!
sleep 10
sudo kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

GZ_MTIME2=$(stat -c %Y "$BASEDIR/test_file.js.gz" 2>/dev/null || echo 0)
BR_MTIME2=$(stat -c %Y "$BASEDIR/test_file.js.br" 2>/dev/null || echo 0)

check "[ $GZ_MTIME1 -eq $GZ_MTIME2 ]" "IDEMP-1: .gz mtime не изменился при повторном запуске"
check "[ $BR_MTIME1 -eq $BR_MTIME2 ]" "IDEMP-2: .br mtime не изменился при повторном запуске"

# ===========================================================================
log "=== Удаление оригинала — сжатые копии удаляются ==="
rm -f "$BASEDIR/test_file.js"
sleep 5

check "[ ! -f '$BASEDIR/test_file.js.gz' ]" "DELETE-1: .gz удалена при удалении оригинала"
check "[ ! -f '$BASEDIR/test_file.js.br' ]" "DELETE-2: .br удалена при удалении оригинала"

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ CONFIGURATIONS: $PASS/$TOTAL пройдено, $FAIL провалено ==="

exit $FAIL
