#!/bin/bash
# Нагрузочное тестирование 1GB+ (ТЗ §18.2)
set -euo pipefail

BASEDIR="/srv/123/loadtest"
SERVICE="mh-compressor-manager"
PASS=0; FAIL=0; TOTAL=0

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }
check() {
    TOTAL=$((TOTAL + 1))
    if bash -c "$1" 2>/dev/null; then
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

# ЭТАП 2: Генерация ~1.2GB текстовых данных (реалистичный веб-контент)
log "=== ЭТАП 2: Генерация тестовых данных (текстовый контент) ==="

# Шаблоны реалистичного контента
HTML_TPL='<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><title>Test Page</title><link rel="stylesheet" href="style.css"></head><body><div class="container"><h1>Welcome to Test Page</h1><p>Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.</p></div></body></html>'
CSS_TPL='* { margin: 0; padding: 0; box-sizing: border-box; } body { font-family: Arial, sans-serif; line-height: 1.6; color: #333; background-color: #fff; } .container { max-width: 1200px; margin: 0 auto; padding: 20px; } h1 { font-size: 2em; margin-bottom: 1em; color: #222; } p { margin-bottom: 1em; } .btn { display: inline-block; padding: 10px 20px; background: #007bff; color: #fff; border: none; border-radius: 4px; cursor: pointer; }'
JS_TPL='(function() { "use strict"; var App = { init: function() { console.log("App initialized"); this.loadData(); }, loadData: function() { fetch("/api/data").then(r => r.json()).then(d => this.render(d)); }, render: function(data) { var el = document.getElementById("root"); if (el) el.innerHTML = JSON.stringify(data); } }; document.addEventListener("DOMContentLoaded", function() { App.init(); }); })();'
JSON_TPL='{"id":1,"name":"Test Entry","description":"This is a test entry for load testing with repeated text data to ensure good compression ratios","tags":["test","load","compression"],"metadata":{"created":"2026-04-14T12:00:00Z","version":"1.0","status":"active"}}'
SVG_TPL='<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><rect x="10" y="10" width="80" height="80" fill="#007bff" stroke="#333" stroke-width="2"/><text x="50" y="55" text-anchor="middle" fill="#fff" font-family="Arial" font-size="14">Test SVG</text></svg>'
TXT_TPL='2026-04-14 12:00:00 [INFO] Request processed: GET /api/data?id=12345 status=200 duration=42ms client=192.168.1.100'

gen_text() {
    local tpl="$1" count_kb="$2"
    local tpl_len=${#tpl}
    local total_chars=$((count_kb * 1024))
    local repeats=$((total_chars / tpl_len + 1))
    for i in $(seq 1 $repeats); do echo "$tpl"; done | head -c "$total_chars"
}

for i in $(seq 1 100); do
    gen_text "$HTML_TPL" $((400 + RANDOM % 200)) > "$BASEDIR/page_${i}.html"
done
log "  100 HTML файлов созданы"

for i in $(seq 1 80); do
    gen_text "$CSS_TPL" $((400 + RANDOM % 200)) > "$BASEDIR/style_${i}.css"
done
log "  80 CSS файлов созданы"

for i in $(seq 1 80); do
    gen_text "$JS_TPL" $((400 + RANDOM % 200)) > "$BASEDIR/script_${i}.js"
done
log "  80 JS файлов созданы"

for i in $(seq 1 80); do
    gen_text "$JSON_TPL" $((400 + RANDOM % 200)) > "$BASEDIR/data_${i}.json"
done
log "  80 JSON файлов созданы"

for i in $(seq 1 200); do
    gen_text "$TXT_TPL" $((4000 + RANDOM % 2000)) > "$BASEDIR/log_${i}.txt"
done
log "  200 TXT файлов созданы"

for i in $(seq 1 20); do
    gen_text "$SVG_TPL" $((800 + RANDOM % 400)) > "$BASEDIR/image_${i}.svg"
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
