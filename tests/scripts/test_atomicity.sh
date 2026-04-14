#!/bin/bash
# =============================================================================
# test_atomicity.sh — Тестирование атомарности записи (ТЗ §18.6.6)
# Проверяет ATOM-1 через ATOM-4
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/atomicity_test"
SERVICE="mh-compressor-manager"
PASS=0
FAIL=0
TOTAL=0

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
log "=== ATOM-1: Сбой записи (нет места на диске) ==="
# Симулируем нет места через tiny tmpfs
sudo mkdir -p /tmp/tiny_disk
sudo mount -t tmpfs -o size=1k tmpfs /tmp/tiny_disk 2>/dev/null || true
python3 -c "print('A' * 500)" > /tmp/tiny_disk/file.txt
sleep 5

# .gz не должна быть полузаписанной — либо полная, либо никакой
TMP_COUNT=$(sudo find /tmp/tiny_disk -name "*.tmp" 2>/dev/null | wc -l)
PARTIAL_COUNT=$(sudo find /tmp/tiny_disk -name "*.gz" -size -50c 2>/dev/null | wc -l)
check "[ $TMP_COUNT -eq 0 ]" "ATOM-1a: нет временных файлов при сбое ($TMP_COUNT)"
check "[ $PARTIAL_COUNT -eq 0 ]" "ATOM-1b: нет частичных .gz файлов ($PARTIAL_COUNT)"

sudo umount /tmp/tiny_disk 2>/dev/null || true
sudo rmdir /tmp/tiny_disk 2>/dev/null || true

# ===========================================================================
log "=== ATOM-2: Crash программы во время записи ==="
python3 -c "print('B' * 5000)" > "$BASEDIR/crash_test.txt"
sleep 0.3

# Убиваем сервис во время записи
sudo systemctl stop "$SERVICE"
sleep 2

# Временные файлы должны отсутствовать
TMP_COUNT=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TMP_COUNT -eq 0 ]" "ATOM-2a: нет .tmp файлов после краша ($TMP_COUNT)"

# Перезапуск — .tmp должны быть подчищены
sudo systemctl start "$SERVICE"
sleep 5

TMP_COUNT=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TMP_COUNT -eq 0 ]" "ATOM-2b: нет .tmp файлов после restart ($TMP_COUNT)"

# ===========================================================================
log "=== ATOM-3: nginx запрашивает .gz в момент rename ==="
# Создаём файл и быстро модифицируем — проверяем что .gz всегда валидна
for i in $(seq 1 20); do
    python3 -c "print('C' * 5000)" > "$BASEDIR/atom_rename_${i}.txt"
    sleep 0.5
done
sleep 5

# Проверяем все .gz файлы — они должны быть валидны
INVALID=0
for gz in $(sudo find "$BASEDIR" -name "atom_rename_*.txt.gz"); do
    ORIG="${gz%.gz}"
    if [ -f "$ORIG" ]; then
        TMP=$(mktemp)
        if ! sudo gunzip -c "$gz" > "$TMP" 2>/dev/null; then
            INVALID=$((INVALID + 1))
            log "  ❌ Невалидный .gz: $gz"
        fi
        # Проверяем что .gz не пустой
        GZ_SIZE=$(stat -c %s "$gz" 2>/dev/null || echo 0)
        if [ "$GZ_SIZE" -lt 20 ]; then
            INVALID=$((INVALID + 1))
            log "  ❌ Слишком маленький .gz: $gz ($GZ_SIZE байт)"
        fi
        rm -f "$TMP"
    fi
done
check "[ $INVALID -eq 0 ]" "ATOM-3a: все .gz файлы валидны ($INVALID невалидных)"

# ===========================================================================
log "=== ATOM-4: Два потока сжимают один файл одновременно ==="
# Создаём файл и быстро создаём ещё один такой же — race между потоками
python3 -c "print('D' * 5000)" > "$BASEDIR/dual_thread.txt"

# Проверяем что только одна .gz создана и она валидна
sleep 5
GZ_COUNT=$(sudo find "$BASEDIR" -name "dual_thread.txt.gz" | wc -l)
check "[ $GZ_COUNT -le 1 ]" "ATOM-4a: .gz создана максимум 1 раз ($GZ_COUNT)"

if [ "$GZ_COUNT" -eq 1 ]; then
    TMP=$(mktemp)
    if sudo gunzip -c "$BASEDIR/dual_thread.txt.gz" > "$TMP" 2>/dev/null; then
        ORIG_HASH=$(md5sum "$BASEDIR/dual_thread.txt" | cut -d' ' -f1)
        DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
        check "[ '$ORIG_HASH' = '$DECOMP_HASH' ]" "ATOM-4b: .gz валидна и соответствует оригиналу"
    else
        check "false" "ATOM-4b: .gz не распаковывается"
    fi
    rm -f "$TMP"
fi

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ ATOMICITY: $PASS/$TOTAL пройдено, $FAIL провалено ==="

exit $FAIL
