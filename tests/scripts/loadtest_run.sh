#!/bin/bash
# Нагрузочное тестирование 1GB+ (ТЗ §18.2)
set -euo pipefail

BASEDIR="/srv/123/loadtest"
SERVICE="mh-compressor-manager"
PASS=0; FAIL=0; TOTAL=0

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }
check() {
    TOTAL=$((TOTAL + 1))
    if eval "$1"; then
        PASS=$((PASS + 1)); echo "  ✅ PASS: $2"
    else
        FAIL=$((FAIL + 1)); echo "  ❌ FAIL: $2"
    fi
}

cleanup() { sudo systemctl stop "$SERVICE" 2>/dev/null || true; }
trap cleanup EXIT

# ЭТАП 1: Подготовка
log "=== ЭТАП 1: Подготовка ==="
sudo systemctl stop "$SERVICE" 2>/dev/null || true
sleep 1
sudo rm -rf "$BASEDIR"
sudo mkdir -p "$BASEDIR"

# ЭТАП 2: Генерация ~1.2GB данных
log "=== ЭТАП 2: Генерация тестовых данных ==="

for i in $(seq 1 100); do
    dd if=/dev/urandom of="$BASEDIR/page_${i}.html" bs=1024 count=$((400 + RANDOM % 200)) 2>/dev/null
done
log "  100 HTML файлов созданы"

for i in $(seq 1 80); do
    dd if=/dev/urandom of="$BASEDIR/style_${i}.css" bs=1024 count=$((400 + RANDOM % 200)) 2>/dev/null
done
log "  80 CSS файлов созданы"

for i in $(seq 1 80); do
    dd if=/dev/urandom of="$BASEDIR/script_${i}.js" bs=1024 count=$((400 + RANDOM % 200)) 2>/dev/null
done
log "  80 JS файлов созданы"

for i in $(seq 1 80); do
    dd if=/dev/urandom of="$BASEDIR/data_${i}.json" bs=1024 count=$((400 + RANDOM % 200)) 2>/dev/null
done
log "  80 JSON файлов созданы"

for i in $(seq 1 200); do
    dd if=/dev/urandom of="$BASEDIR/log_${i}.txt" bs=1024 count=$((4000 + RANDOM % 2000)) 2>/dev/null
done
log "  200 TXT файлов созданы"

for i in $(seq 1 20); do
    dd if=/dev/urandom of="$BASEDIR/image_${i}.svg" bs=1024 count=$((800 + RANDOM % 400)) 2>/dev/null
done
log "  20 SVG файлов созданы"

for i in $(seq 1 50); do
    dd if=/dev/urandom of="$BASEDIR/small_${i}.js" bs=1 count=$((50 + RANDOM % 150)) 2>/dev/null
done
log "  50 мелких файлов (< 256 байт) созданы"

FILE_COUNT=$(sudo find "$BASEDIR" -type f | wc -l)
TOTAL_SIZE=$(sudo du -sm "$BASEDIR" | cut -f1)
log "  ИТОГО: $FILE_COUNT файлов, ${TOTAL_SIZE}MB (требуется >=1024MB по ТЗ)"
check "[ $TOTAL_SIZE -ge 1024 ]" "Объём данных >= 1GB: ${TOTAL_SIZE}MB"

# ЭТАП 3: Stale detection
log "=== ЭТАП 3: Stale detection ==="
sudo find "$BASEDIR" -type f \( -name '*.html' -o -name '*.css' -o -name '*.js' \) | shuf | head -100 | while read -r f; do
    [ -f "$f" ] || continue
    sudo gzip -k "$f" 2>/dev/null || true
    sudo touch -t 202501010000 "${f}.gz" 2>/dev/null || true
    sudo brotli -k "$f" 2>/dev/null || true
    sudo touch -t 202501010000 "${f}.br" 2>/dev/null || true
done
STALE_GZ=$(sudo find "$BASEDIR" -name '*.gz' | wc -l)
STALE_BR=$(sudo find "$BASEDIR" -name '*.br' | wc -l)
log "  Создано $STALE_GZ .gz и $STALE_BR .br stale файлов"
check "[ $STALE_GZ -ge 50 ]" "Stale .gz файлов >= 50: $STALE_GZ"

# ЭТАП 4: Запуск сервиса с мониторингом
log "=== ЭТАП 4: Запуск сервиса с мониторингом ==="
sudo systemctl start "$SERVICE"
START_TIME=$(date +%s)
MEM_PEAK=0

for i in $(seq 1 60); do
    sleep 10

    # Логирование памяти каждые 30 секунд
    MEM_RAW=$(systemctl show "$SERVICE" --property=MemoryCurrent --value 2>/dev/null || echo "0")
    if [ "$MEM_RAW" -gt 0 ] 2>/dev/null; then
        MEM_MB=$((MEM_RAW / 1048576))
        if [ $MEM_MB -gt $MEM_PEAK ]; then
            MEM_PEAK=$MEM_MB
        fi
        if [ $((i % 3)) -eq 0 ]; then
            log "  💾 ПАМЯТЬ: ${MEM_MB}MB (пик: ${MEM_PEAK}MB)"
        fi
    fi

    GZ_COUNT=$(sudo find "$BASEDIR" -name '*.gz' 2>/dev/null | wc -l)
    BR_COUNT=$(sudo find "$BASEDIR" -name '*.br' 2>/dev/null | wc -l)
    log "  [${i}0s] .gz=$GZ_COUNT, .br=$BR_COUNT"

    EXPECTED=$((FILE_COUNT - 50))
    if [ "$GZ_COUNT" -ge "$EXPECTED" ] && [ "$BR_COUNT" -ge "$EXPECTED" ]; then
        log "  Все файлы обработаны за ${i}0 секунд"
        break
    fi
done

ELAPSED=$(( $(date +%s) - START_TIME ))
log "  Общее время: ${ELAPSED} секунд"
log "  Пиковая память: ${MEM_PEAK}MB"
check "[ $ELAPSED -lt 600 ]" "Время обработки < 10 минут: ${ELAPSED}с"

# ЭТАП 5: Валидация сжатия
log "=== ЭТАП 5: Валидация gzip/brotli ==="
GZ_OK=0; GZ_FAIL=0
for gz in $(sudo find "$BASEDIR" -name '*.gz' | shuf | head -20); do
    ORIG="${gz%.gz}"
    [ -f "$ORIG" ] || continue
    TMP=$(mktemp)
    if sudo gunzip -c "$gz" > "$TMP" 2>/dev/null; then
        ORIG_HASH=$(md5sum "$ORIG" | cut -d' ' -f1)
        DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
        if [ "$ORIG_HASH" = "$DECOMP_HASH" ]; then
            GZ_OK=$((GZ_OK + 1))
        else
            GZ_FAIL=$((GZ_FAIL + 1))
        fi
    else
        GZ_FAIL=$((GZ_FAIL + 1))
    fi
    rm -f "$TMP"
done
check "[ $GZ_FAIL -eq 0 ]" "Gzip валидация: $GZ_OK OK, $GZ_FAIL ошибок"

BR_OK=0; BR_FAIL=0
for br in $(sudo find "$BASEDIR" -name '*.br' | shuf | head -20); do
    ORIG="${br%.br}"
    [ -f "$ORIG" ] || continue
    TMP=$(mktemp)
    if sudo brotli -d -c "$br" > "$TMP" 2>/dev/null; then
        ORIG_HASH=$(md5sum "$ORIG" | cut -d' ' -f1)
        DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
        if [ "$ORIG_HASH" = "$DECOMP_HASH" ]; then
            BR_OK=$((BR_OK + 1))
        else
            BR_FAIL=$((BR_FAIL + 1))
        fi
    else
        BR_FAIL=$((BR_FAIL + 1))
    fi
    rm -f "$TMP"
done
check "[ $BR_FAIL -eq 0 ]" "Brotli валидация: $BR_OK OK, $BR_FAIL ошибок"

# ЭТАП 6: Проверка памяти
log "=== ЭТАП 6: Проверка потребления памяти ==="
if [ "$MEM_PEAK" -gt 0 ]; then
    log "  Пиковое потребление: ${MEM_PEAK}MB (по данным systemd)"
    check "[ $MEM_PEAK -le 200 ]" "Память <= 200MB: ${MEM_PEAK}MB"
else
    MEM=$(systemctl show "$SERVICE" --property=MemoryCurrent --value 2>/dev/null || echo "0")
    if [ "$MEM" -gt 0 ] 2>/dev/null; then
        MEM_MB=$((MEM / 1048576))
        log "  Потребление памяти: ${MEM_MB}MB"
        check "[ $MEM_MB -le 200 ]" "Память <= 200MB: ${MEM_MB}MB"
    else
        log "  Потребление памяти: N/A (сервис остановлен)"
    fi
fi

# ЭТАП 7: Итоги
log "=== ИТОГИ НАГРУЗОЧНОГО ТЕСТИРОВАНИЯ ==="
log "  Пройдено: $PASS/$TOTAL"
log "  Провалено: $FAIL/$TOTAL"

FINAL_GZ=$(sudo find "$BASEDIR" -name '*.gz' | wc -l)
FINAL_BR=$(sudo find "$BASEDIR" -name '*.br' | wc -l)
log "  Сжатые файлы: .gz=$FINAL_GZ, .br=$FINAL_BR"
check "[ $FINAL_GZ -ge $((FILE_COUNT - 50)) ]" "Все файлы сжаты в .gz: $FINAL_GZ"
check "[ $FINAL_BR -ge $((FILE_COUNT - 50)) ]" "Все файлы сжаты в .br: $FINAL_BR"

if [ $FAIL -eq 0 ]; then
    log "✅ НАГРУЗОЧНОЕ ТЕСТИРОВАНИЕ ПРОЙДЕНО ПОЛНОСТЬЮ"
else
    log "❌ НАГРУЗОЧНОЕ ТЕСТИРОВАНИЕ: $FAIL ПРОВАЛЕНО"
fi
