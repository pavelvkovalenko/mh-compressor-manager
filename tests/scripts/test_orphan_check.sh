#!/bin/bash
# =============================================================================
# test_orphan_check.sh — Проверка отсутствия orphan-файлов (ТЗ §18.6.6)
# Проверяет ORPHAN-1 через ORPHAN-3
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/orphan_test"
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
sudo systemctl start "$SERVICE"
sleep 2

# ===========================================================================
log "=== ORPHAN-1: После 100 операций — нет .gz/.br без оригинала ==="

# Создаём 50 файлов и сжимаем
for i in $(seq 1 50); do
    python3 -c "print('A' * 500)" > "$BASEDIR/orphan_test_${i}.txt"
done
sleep 10

# Удаляем 30 оригиналов
for i in $(seq 1 30); do
    rm -f "$BASEDIR/orphan_test_${i}.txt"
done
sleep 10

# Проверяем orphan
ORPHANS=$(sudo find "$BASEDIR" \( -name "*.gz" -o -name "*.br" \) -exec sh -c '
    ORIG="${1%.gz}"
    ORIG="${ORIG%.br}"
    [ ! -f "$ORIG" ] && echo "$1"
' _ {} \; | wc -l)

check "[ $ORPHANS -eq 0 ]" "ORPHAN-1a: $ORPHANS orphan сжатых файлов после удалений"

# ===========================================================================
log "=== ORPHAN-2: После 100 операций — нет .tmp файлов ==="

# Интенсивная модификация
for i in $(seq 1 100); do
    python3 -c "print('B' * 500)" > "$BASEDIR/tmp_test_${i}.txt"
    sleep 0.05
done
sleep 15

TMP_COUNT=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TMP_COUNT -eq 0 ]" "ORPHAN-2a: $TMP_COUNT временных файлов после 100 операций"

# ===========================================================================
log "=== ORPHAN-3: После серии перемещений — нет stale на старых путях ==="

sudo mkdir -p "$BASEDIR/dir_a" "$BASEDIR/dir_b" "$BASEDIR/dir_c"

# Создаём файлы в dir_a
for i in $(seq 1 20); do
    python3 -c "print('C' * 500)" > "$BASEDIR/dir_a/move_${i}.txt"
done
sleep 10

# Перемещаем dir_a → dir_b
mv "$BASEDIR/dir_a" "$BASEDIR/dir_b/nested"
sleep 10

# Перемещаем nested → dir_c
mv "$BASEDIR/dir_b/nested" "$BASEDIR/dir_c/final"
sleep 10

# Проверяем stale на старых путях
STALE_A=$(sudo find "$BASEDIR/dir_a" -name "*.gz" 2>/dev/null | wc -l)
STALE_B=$(sudo find "$BASEDIR/dir_b/nested" -name "*.gz" 2>/dev/null | wc -l)
STALE_C=$(sudo find "$BASEDIR/dir_c/final" -name "*.gz" 2>/dev/null | wc -l)
FINAL_TOTAL=$(sudo find "$BASEDIR/dir_c/final" -name "*.gz" 2>/dev/null | wc -l)

check "[ $STALE_A -eq 0 ]" "ORPHAN-3a: 0 stale в dir_a (старый путь)"
check "[ $STALE_B -eq 0 ]" "ORPHAN-3b: 0 stale в dir_b/nested (промежуточный)"
check "[ $FINAL_TOTAL -ge 15 ]" "ORPHAN-3c: $FINAL_TOTAL/20 .gz в final (актуальный)"

# ===========================================================================
log "=== Финальная полная проверка ==="
TOTAL_ORPHANS=$(sudo find "$BASEDIR" \( -name "*.gz" -o -name "*.br" \) -exec sh -c '
    ORIG="${1%.gz}"
    ORIG="${ORIG%.br}"
    [ ! -f "$ORIG" ] && echo "$1"
' _ {} \; | wc -l)

TOTAL_TMP=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)

log "  Orphan сжатых файлов: $TOTAL_ORPHANS"
log "  Временных файлов: $TOTAL_TMP"
check "[ $TOTAL_ORPHANS -eq 0 -a $TOTAL_TMP -eq 0 ]" "ORPHAN-FINAL: Нет orphan и tmp файлов"

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ ORPHAN CHECK: $PASS/$TOTAL пройдено, $FAIL провалено ==="

exit $FAIL
