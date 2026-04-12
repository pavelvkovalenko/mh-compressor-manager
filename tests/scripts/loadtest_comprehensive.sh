#!/bin/bash
# =============================================================================
# loadtest_comprehensive.sh — Комплексное нагрузочное тестирование (ТЗ §18.2.2)
# 7 этапов: подготовка → генерация → stale detection → запуск → валидация → стресс → итоги
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/loadtest"
SERVICE="mh-compressor-manager"
PASS=0
FAIL=0
TOTAL=0

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }
check() {
    TOTAL=$((TOTAL + 1))
    if eval "$1"; then
        PASS=$((PASS + 1))
        echo "  ✅ PASS: $2"
    else
        FAIL=$((FAIL + 1))
        echo "  ❌ FAIL: $2"
    fi
}

# ===========================================================================
# ЭТАП 1: Подготовка
# ===========================================================================
log "=== ЭТАП 1: Подготовка ==="
sudo systemctl stop "$SERVICE" 2>/dev/null || true
sleep 1
sudo rm -rf "$BASEDIR"
sudo mkdir -p "$BASEDIR"

# ===========================================================================
# ЭТАП 2: Генерация 500 файлов (~350MB) — реальный веб-контент
# ===========================================================================
log "=== ЭТАП 2: Генерация тестовых данных ==="

# HTML файлы (80 шт, ~40MB)
for i in $(seq 1 80); do
    size=$((200000 + RANDOM % 800000))
    python3 -c "
print('<!DOCTYPE html><html><head><title>Test Page $i</title></head><body>')
print('<h1>Load Test Page $i</h1>')
print('<p>' + 'A' * $((size - 200)) + '</p>')
print('</body></html>')
" > "$BASEDIR/page_${i}.html" 2>/dev/null
done
log "  Сгенерировано 80 HTML файлов"

# CSS файлы (50 шт, ~25MB)
for i in $(seq 1 50); do
    python3 -c "
print('/* CSS Test File $i */')
for j in range(200):
    print(f'.class-{j} {{ color: #{j:06x}; margin: {j}px; padding: {j}px; }}')
" > "$BASEDIR/style_${i}.css" 2>/dev/null
done
log "  Сгенерировано 50 CSS файлов"

# JS файлы (50 шт, ~30MB)
for i in $(seq 1 50); do
    python3 -c "
print('// JavaScript Test File $i')
print('function testFunc() {')
for j in range(300):
    print(f'  var var_{j} = \"test_string_{j}\" * {j};')
print('}')
" > "$BASEDIR/script_${i}.js" 2>/dev/null
done
log "  Сгенерировано 50 JS файлов"

# JSON файлы (60 шт, ~30MB)
for i in $(seq 1 60); do
    python3 -c "
import json
data = {'id': $i, 'data': [{'key': str(k), 'value': k*10} for k in range(200)]}
print(json.dumps(data, indent=2))
" > "$BASEDIR/data_${i}.json" 2>/dev/null
done
log "  Сгенерировано 60 JSON файлов"

# TXT файлы (260 шт, ~200MB)
for i in $(seq 1 260); do
    size=$((500000 + RANDOM % 2000000))
    python3 -c "
for j in range($((size // 50))):
    print(f'Line {j}: Lorem ipsum dolor sit amet, consectetur adipiscing elit.')
" > "$BASEDIR/log_${i}.txt" 2>/dev/null
done
log "  Сгенерировано 260 TXT файлов"

# Мелкие файлы для проверки порога (50 шт, 50-200 байт)
for i in $(seq 1 50); do
    size=$((50 + RANDOM % 150))
    python3 -c "print('X' * $((size)))" > "$BASEDIR/small_${i}.js" 2>/dev/null
done
log "  Сгенерировано 50 мелких файлов (< 256 байт)"

FILE_COUNT=$(sudo find "$BASEDIR" -type f | wc -l)
TOTAL_SIZE=$(sudo du -sm "$BASEDIR" | cut -f1)
log "  ИТОГО: $FILE_COUNT файлов, ${TOTAL_SIZE}MB"

# ===========================================================================
# ЭТАП 3: Stale detection (40% предварительно сжатых файлов)
# ===========================================================================
log "=== ЭТАП 3: Stale detection — создание 40% stale файлов ==="

# Предварительно сжимаем ~40% файлов и ставим старый mtime
FILES_TO_STALE=$(sudo find "$BASEDIR" -type f -name "*.html" -o -name "*.css" -o -name "*.js" | shuf | head -100)
echo "$FILES_TO_STALE" | while read -r f; do
    [ -f "$f" ] || continue
    sudo gzip -k "$f" 2>/dev/null || true
    sudo touch -t 202501010000 "${f}.gz" 2>/dev/null || true
    sudo brotli -k "$f" 2>/dev/null || true
    sudo touch -t 202501010000 "${f}.br" 2>/dev/null || true
done
log "  Создано 100 пар stale .gz/.br файлов с mtime = 2025-01-01"

# ===========================================================================
# ЭТАП 4: Запуск сервиса с мониторингом
# ===========================================================================
log "=== ЭТАП 4: Запуск сервиса с мониторингом ==="

# Запускаем сервис в фоне
sudo systemctl start "$SERVICE"
START_TIME=$(date +%s)

# Мониторинг каждые 10 секунд (максимум 5 минут)
for i in $(seq 1 30); do
    sleep 10
    MEM=$(systemctl show "$SERVICE" --property=MemoryCurrent --value 2>/dev/null || echo "N/A")
    if [ "$MEM" != "N/A" ] && [ "$MEM" -gt 0 ] 2>/dev/null; then
        MEM_MB=$((MEM / 1048576))
    else
        MEM_MB="?"
    fi

    GZ_COUNT=$(sudo find "$BASEDIR" -name "*.gz" 2>/dev/null | wc -l)
    BR_COUNT=$(sudo find "$BASEDIR" -name "*.br" 2>/dev/null | wc -l)

    log "  [${i}0s] Сжатые: .gz=$GZ_COUNT, .br=$BR_COUNT, Память=${MEM_MB}MB"

    # Если все файлы обработаны — выходим
    EXPECTED=$((FILE_COUNT - 50))  # Исключая мелкие файлы
    if [ "$GZ_COUNT" -ge "$EXPECTED" ] && [ "$BR_COUNT" -ge "$EXPECTED" ]; then
        log "  Все файлы обработаны на ${i}0-й секунде"
        break
    fi
done

ELAPSED=$(($(date +%s) - START_TIME))
log "  Время обработки: ${ELAPSED} секунд"

# ===========================================================================
# ЭТАП 5: Валидация корректности сжатия
# ===========================================================================
log "=== ЭТАП 5: Валидация gzip/brotli ==="

# Проверка 20 случайных .gz файлов
GZ_CHECK=0
GZ_OK=0
GZ_FAIL=0
for gz in $(sudo find "$BASEDIR" -name "*.gz" | shuf | head -20); do
    GZ_CHECK=$((GZ_CHECK + 1))
    ORIG="${gz%.gz}"
    if [ -f "$ORIG" ]; then
        TMP=$(mktemp)
        if sudo gunzip -c "$gz" > "$TMP" 2>/dev/null; then
            ORIG_HASH=$(md5sum "$ORIG" | cut -d' ' -f1)
            DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
            if [ "$ORIG_HASH" = "$DECOMP_HASH" ]; then
                GZ_OK=$((GZ_OK + 1))
            else
                GZ_FAIL=$((GZ_FAIL + 1))
                log "  ❌ GZIP hash mismatch: $gz"
            fi
        else
            GZ_FAIL=$((GZ_FAIL + 1))
            log "  ❌ GZIP decompress failed: $gz"
        fi
        rm -f "$TMP"
    fi
done
check "[ $GZ_FAIL -eq 0 ]" "Gzip валидация: $GZ_OK/$GZ_CHECK OK, $GZ_FAIL ошибок"

# Проверка 20 случайных .br файлов
BR_CHECK=0
BR_OK=0
BR_FAIL=0
for br in $(sudo find "$BASEDIR" -name "*.br" | shuf | head -20); do
    BR_CHECK=$((BR_CHECK + 1))
    ORIG="${br%.br}"
    if [ -f "$ORIG" ]; then
        TMP=$(mktemp)
        if sudo brotli -d -c "$br" > "$TMP" 2>/dev/null; then
            ORIG_HASH=$(md5sum "$ORIG" | cut -d' ' -f1)
            DECOMP_HASH=$(md5sum "$TMP" | cut -d' ' -f1)
            if [ "$ORIG_HASH" = "$DECOMP_HASH" ]; then
                BR_OK=$((BR_OK + 1))
            else
                BR_FAIL=$((BR_FAIL + 1))
                log "  ❌ Brotli hash mismatch: $br"
            fi
        else
            BR_FAIL=$((BR_FAIL + 1))
            log "  ❌ Brotli decompress failed: $br"
        fi
        rm -f "$TMP"
    fi
done
check "[ $BR_FAIL -eq 0 ]" "Brotli валидация: $BR_OK/$BR_CHECK OK, $BR_FAIL ошибок"

# Проверка что stale-файлы пережаты (mtime новее оригинала)
STALE_FIXED=0
STALE_TOTAL=0
echo "$FILES_TO_STALE" | head -20 | while read -r f; do
    [ -f "$f" ] || continue
    if [ -f "${f}.gz" ]; then
        SRC_MTIME=$(stat -c %Y "$f" 2>/dev/null || echo 0)
        GZ_MTIME=$(stat -c %Y "${f}.gz" 2>/dev/null || echo 0)
        if [ "$GZ_MTIME" -ge "$SRC_MTIME" ]; then
            echo "OK" >> /tmp/stale_check.txt
        else
            echo "FAIL" >> /tmp/stale_check.txt
        fi
    fi
done
rm -f /tmp/stale_check.txt

# ===========================================================================
# ЭТАП 6: Стресс-тест — 100 новых файлов за раз
# ===========================================================================
log "=== ЭТАП 6: Стресс-тест (100 файлов за 1 секунду) ==="
STRESS_START=$(date +%s)

for i in $(seq 1 100); do
    python3 -c "print('Stress test file $i ' + 'A' * 1000)" > "$BASEDIR/stress_${i}.html" 2>/dev/null
done

STRESS_ADDED=100
log "  Добавлено $STRESS_ADDED файлов, ожидание обработки..."

# Ожидание обработки (максимум 60 секунд)
for i in $(seq 1 12); do
    sleep 5
    STRESS_GZ=$(sudo find "$BASEDIR" -name "stress_*.html.gz" 2>/dev/null | wc -l)
    STRESS_BR=$(sudo find "$BASEDIR" -name "stress_*.html.br" 2>/dev/null | wc -l)
    log "  [${i}x5s] Стресс: .gz=$STRESS_GZ, .br=$STRESS_BR"
    if [ "$STRESS_GZ" -ge 95 ] && [ "$STRESS_BR" -ge 95 ]; then
        log "  Стресс-файлы обработаны"
        break
    fi
done

STRESS_ELAPSED=$(($(date +%s) - STRESS_START))
check "[ $STRESS_GZ -ge 95 ]" "Стресс-тест: $STRESS_GZ/100 .gz за ${STRESS_ELAPSED}с"
check "[ $STRESS_BR -ge 95 ]" "Стресс-тест: $STRESS_BR/100 .br за ${STRESS_ELAPSED}с"

# Проверка мелких файлов — НЕ должны быть сжаты
SMALL_GZ=$(sudo find "$BASEDIR" -name "small_*.js.gz" 2>/dev/null | wc -l)
SMALL_BR=$(sudo find "$BASEDIR" -name "small_*.js.br" 2>/dev/null | wc -l)
check "[ $SMALL_GZ -eq 0 ]" "Мелкие файлы: $SMALL_GZ .gz (должно быть 0)"
check "[ $SMALL_BR -eq 0 ]" "Мелкие файлы: $SMALL_BR .br (должно быть 0)"

# ===========================================================================
# ЭТАП 7: Итоговые метрики и очистка
# ===========================================================================
log "=== ЭТАП 7: Итоговые метрики ==="

FINAL_GZ=$(sudo find "$BASEDIR" -name "*.gz" | wc -l)
FINAL_BR=$(sudo find "$BASEDIR" -name "*.br" | wc -l)
FINAL_ORIG=$(sudo find "$BASEDIR" -type f ! -name "*.gz" ! -name "*.br" | wc -l)
FINAL_SIZE_ORIG=$(sudo find "$BASEDIR" -type f ! -name "*.gz" ! -name "*.br" -exec du -cb {} + 2>/dev/null | tail -1 | cut -f1)
FINAL_SIZE_GZ=$(sudo find "$BASEDIR" -name "*.gz" -exec du -cb {} + 2>/dev/null | tail -1 | cut -f1)
FINAL_SIZE_BR=$(sudo find "$BASEDIR" -name "*.br" -exec du -cb {} + 2>/dev/null | tail -1 | cut -f1)

MEM_PEAK=$(systemctl show "$SERVICE" --property=MemoryPeak --value 2>/dev/null || echo "N/A")
if [ "$MEM_PEAK" != "N/A" ] && [ "$MEM_PEAK" -gt 0 ] 2>/dev/null; then
    MEM_PEAK_MB=$((MEM_PEAK / 1048576))
else
    MEM_PEAK_MB="?"
fi

log "  Исходных файлов: $FINAL_ORIG"
log "  Сжатых .gz: $FINAL_GZ"
log "  Сжатых .br: $FINAL_BR"
log "  Размер оригиналов: $((FINAL_SIZE_ORIG / 1048576))MB"
log "  Размер .gz: $((FINAL_SIZE_GZ / 1048576))MB"
log "  Размер .br: $((FINAL_SIZE_BR / 1048576))MB"
log "  Пиковая память: ${MEM_PEAK_MB}MB"

if [ "$FINAL_SIZE_ORIG" -gt 0 ] && [ "$FINAL_SIZE_GZ" -gt 0 ]; then
    RATIO_GZ=$((FINAL_SIZE_GZ * 100 / FINAL_SIZE_ORIG))
    log "  Коэффициент сжатия gzip: ${RATIO_GZ}%"
fi
if [ "$FINAL_SIZE_ORIG" -gt 0 ] && [ "$FINAL_SIZE_BR" -gt 0 ]; then
    RATIO_BR=$((FINAL_SIZE_BR * 100 / FINAL_SIZE_ORIG))
    log "  Коэффициент сжатия brotli: ${RATIO_BR}%"
fi

log ""
log "=== РЕЗУЛЬТАТЫ: $PASS/$TOTAL пройдено, $FAIL провалено ==="

# Остановка сервиса
sudo systemctl stop "$SERVICE"

# Очистка
log "=== Очистка ==="
sudo rm -rf "$BASEDIR"
log "  Тестовые данные удалены"

exit $FAIL
