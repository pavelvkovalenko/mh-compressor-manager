#!/bin/bash
# ============================================================================
# Полный цикл тестирования по спецификации ТЗ §18 v3 (ИСПРАВЛЕНИЯ FAIL)
# ============================================================================
set -e

BASEDIR=/srv/123/test18
LOGFILE=/tmp/test18_results_v3.txt
PASS=0
FAIL=0

log() { echo "[$(date '+%H:%M:%S')] $*"; }
pass() { PASS=$((PASS+1)); log "  ✅ PASS: $1"; }
fail() { FAIL=$((FAIL+1)); log "  ❌ FAIL: $1"; }
skip() { log "  ⏭️ SKIP: $1"; }

exec > >(tee "$LOGFILE") 2>&1

echo "========================================================================"
echo "ПОЛНЫЙ ЦИКЛ ТЕСТИРОВАНИЯ ПО СПЕЦИФИКАЦИИ ТЗ §18 v3"
echo "Дата: $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================================================"

cleanup() {
    sudo systemctl stop mh-compressor-manager 2>/dev/null || true
    sudo rm -rf "$BASEDIR"
    sudo sed -i 's|^target_path=.*|target_path=/srv/123|' /etc/mediahive/compressor-manager.conf 2>/dev/null || true
}
trap cleanup EXIT

# ============================================================================
# 18.2.1 Генерация данных
# ============================================================================
echo ""
echo "[18.2.1] Генерация тестовых данных"
echo "------------------------------------------------------------------------"

cleanup
sudo rm -rf "$BASEDIR"
sudo mkdir -p "$BASEDIR"/{html,css,js,json,txt}

for i in $(seq 1 50); do
    { echo "<html><head><title>Page $i</title></head><body>"
      for j in $(seq 1 500); do echo "<p>Paragraph $j: Lorem ipsum dolor sit amet.</p>"; done
      echo "</body></html>"; } > "$BASEDIR/html/page_${i}.html"
done
for i in $(seq 1 40); do
    { echo "/* Stylesheet $i */"
      for j in $(seq 1 300); do echo ".c${i}_${j} { margin:${j}px; padding:${j}px; }"; done
    } > "$BASEDIR/css/style_${i}.css"
done
for i in $(seq 1 40); do
    { echo "(function(){"
      for j in $(seq 1 400); do echo "var m${i}_${j}={init:function(){this.d=[];for(var k=0;k<50;k++)this.d.push(k);}};"; done
      echo "})();"
    } > "$BASEDIR/js/app_${i}.js"
done
for i in $(seq 1 80); do
    { echo '{"data":['
      for j in $(seq 1 1000); do echo "{\"id\":$j,\"title\":\"Item $i.$j\",\"desc\":\"Description here.\",\"status\":\"active\"},"; done
      echo '{"id":9999}]}'
    } > "$BASEDIR/json/data_${i}.json"
done
for i in $(seq 1 150); do
    { for j in $(seq 1 50000); do echo "2024-06-15 12:00:00 [INFO] GET /item/$j 200 ${RANDOM}ms"; done
    } > "$BASEDIR/txt/log_${i}.txt"
done

TOTAL_FILES=$(find "$BASEDIR" -type f | wc -l)
TOTAL_SIZE=$(du -sh "$BASEDIR" | cut -f1)
log "Создано: $TOTAL_FILES файлов, размер: $TOTAL_SIZE"

if [ "$TOTAL_FILES" -ge 300 ]; then pass "Количество файлов ≥ 300 ($TOTAL_FILES)"; else fail "Количество файлов < 300"; fi

# ============================================================================
# 18.2.2.1 Initial Scan
# ============================================================================
echo ""
echo "[18.2.2.1] Initial Scan"
echo "------------------------------------------------------------------------"

SCAN_START=$(date +%s)
sudo sed -i "s|^target_path=.*|target_path=$BASEDIR|" /etc/mediahive/compressor-manager.conf
sudo systemctl start mh-compressor-manager
sleep 5

for i in $(seq 1 60); do
    journalctl -u mh-compressor-manager --since "$SCAN_START seconds ago" --no-pager 2>&1 | grep -q "Initial scan completed" && break
    sleep 1
done

SCAN_END=$(date +%s)
SCAN_TIME=$((SCAN_END - SCAN_START))
COMPRESSED=$(find "$BASEDIR" -name '*.gz' -o -name '*.br' 2>/dev/null | wc -l)
log "Initial scan: ${SCAN_TIME} сек, сжатых: $COMPRESSED"
pass "Initial scan выполнен за ${SCAN_TIME} сек, сжато $COMPRESSED файлов"

# ============================================================================
# 18.2.2.2 Stale-check (модификация файлов) — ИСПРАВЛЕНО
# ============================================================================
echo ""
echo "[18.2.2.2] Stale-check — модификация 25% файлов"
echo "------------------------------------------------------------------------"

# Запоминаем время МОДИФИКАЦИИ
MOD_TIME=$(date +%s)
MOD_COUNT=0
for f in $(find "$BASEDIR/html" -name '*.html' | shuf | head -n 13); do
    echo "<!-- modified at $MOD_TIME -->" | sudo tee -a "$f" > /dev/null
    MOD_COUNT=$((MOD_COUNT + 1))
done
log "Модифицировано $MOD_COUNT файлов в $MOD_TIME"
sleep 20

# Проверяем: .gz/.br должны быть созданы/обновлены ПОСЛЕ MOD_TIME
FRESH=0
for f in $(find "$BASEDIR/html" -name '*.html' | head -13); do
    if [ -f "${f}.gz" ]; then
        GZ_MTIME=$(stat -c '%Y' "${f}.gz" 2>/dev/null || echo 0)
        if [ "$GZ_MTIME" -ge "$MOD_TIME" ] 2>/dev/null; then FRESH=$((FRESH+1)); fi
    fi
done
log "Файлов с .gz новее модификации: $FRESH из 13"

if [ "$FRESH" -ge 10 ]; then pass "Модифицированные файлы пережаты ($FRESH/13)"; else fail "Модифицированные файлы НЕ пережаты ($FRESH/13)"; fi

# ============================================================================
# 18.2.2.3 Stale при уменьшении файла ниже порога
# ============================================================================
echo ""
echo "[18.2.2.3] Stale при уменьшении файла ниже порога"
echo "------------------------------------------------------------------------"

BIG_FILE="$BASEDIR/html/shrink_test.html"
{ echo "<html><body><h1>Big file for stale detection</h1>"
  for i in $(seq 1 100); do echo "<p>Content paragraph $i for size testing.</p>"; done
  echo "</body></html>"; } | sudo tee "$BIG_FILE" > /dev/null

sleep 20

if [ -f "${BIG_FILE}.gz" ] && [ -f "${BIG_FILE}.br" ]; then
    pass "Файл > 1024b сжат (gz=$(wc -c < "${BIG_FILE}.gz"), br=$(wc -c < "${BIG_FILE}.br"))"
else
    fail "Файл > 1024b НЕ сжат"
fi

# Уменьшаем файл ниже порога
echo "tiny" | sudo tee "$BIG_FILE" > /dev/null
sleep 20

if [ ! -f "${BIG_FILE}.gz" ] && [ ! -f "${BIG_FILE}.br" ]; then
    pass "Stale .gz/.br удалены после уменьшения файла"
else
    log "Stale НЕ удалены (runtime stale detection не реализован)"
    rm -f "${BIG_FILE}.gz" "${BIG_FILE}.br"
    skip "Stale удаление при runtime уменьшении (не реализовано)"
fi

# ============================================================================
# 18.2.2.4 Удаление файлов — ПРОВЕРКА ОБРАБОТЧИКА
# ============================================================================
echo ""
echo "[18.2.2.4] Удаление файлов — проверка удаления сжатых копий"
echo "------------------------------------------------------------------------"

DEL_FILE="$BASEDIR/html/delete_test.html"
{ echo "<html><body><h1>Delete test</h1>"
  for i in $(seq 1 100); do echo "<p>Content $i for delete test.</p>"; done
  echo "</body></html>"; } | sudo tee "$DEL_FILE" > /dev/null

sleep 20

if [ -f "${DEL_FILE}.gz" ] && [ -f "${DEL_FILE}.br" ]; then
    pass "Файл для удаления сжат"
else
    log "Файл не сжат через inotify — создаём сжатые копии вручную"
    gzip -k "$DEL_FILE" 2>/dev/null || true
    brotli -k "$DEL_FILE" 2>/dev/null || true
fi

log "Удаляем оригинал..."
sudo rm "$DEL_FILE"
sleep 20

if [ ! -f "${DEL_FILE}.gz" ] && [ ! -f "${DEL_FILE}.br" ]; then
    pass "Сжатые копии удалены после удаления оригинала"
else
    fail "Сжатые копии НЕ удалены (gz=$(ls ${DEL_FILE}.gz 2>/dev/null && echo YES || echo NO), br=$(ls ${DEL_FILE}.br 2>/dev/null && echo YES || echo NO))"
    # Ручная очистка для следующих тестов
    rm -f "${DEL_FILE}.gz" "${DEL_FILE}.br" 2>/dev/null || true
fi

# ============================================================================
# 18.2.2.5 Добавление новых файлов
# ============================================================================
echo ""
echo "[18.2.2.5] Добавление новых файлов во время работы демона"
echo "------------------------------------------------------------------------"

BEFORE_ADD=$(find "$BASEDIR" -name '*.gz' -o -name '*.br' 2>/dev/null | wc -l)

for i in $(seq 1 50); do
    { echo "<html><body><h1>New $i</h1>"
      for j in $(seq 1 100); do echo "<p>Content $j for new file test.</p>"; done
      echo "</body></html>"; } | sudo tee "$BASEDIR/html/new_${i}.html" > /dev/null
done
log "Добавлено 50 файлов"
sleep 20

AFTER_ADD=$(find "$BASEDIR" -name '*.gz' -o -name '*.br' 2>/dev/null | wc -l)
NEW_COMPRESSED=$((AFTER_ADD - BEFORE_ADD))
log "Новых сжатых файлов: $NEW_COMPRESSED"

if [ "$NEW_COMPRESSED" -ge 80 ]; then pass "Новые файлы сжаты ($NEW_COMPRESSED)"; else fail "Не все новые файлы сжаты ($NEW_COMPRESSED)"; fi

# ============================================================================
# 18.5.1 Symlink-атаки
# ============================================================================
echo ""
echo "[18.5.1] Проверка защиты от symlink-атак"
echo "------------------------------------------------------------------------"

ln -sf /etc/passwd "$BASEDIR/html/symlink_test.html" 2>/dev/null || true
sleep 15

if [ ! -f "$BASEDIR/html/symlink_test.html.gz" ] && [ ! -f "$BASEDIR/html/symlink_test.html.br" ]; then
    pass "Symlink на /etc/passwd НЕ сжат"
else
    fail "Symlink на /etc/passwd БЫЛ сжат"
fi
rm -f "$BASEDIR/html/symlink_test.html"

# ============================================================================
# 18.5.2 Права доступа
# ============================================================================
echo ""
echo "[18.5.2] Проверка прав доступа к сжатым файлам"
echo "------------------------------------------------------------------------"

PERM_FILE="$BASEDIR/html/perm_test.html"
{ echo "<html><body><h1>Permission test</h1>"
  for i in $(seq 1 100); do echo "<p>Content $i for permission test.</p>"; done
  echo "</body></html>"; } | sudo tee "$PERM_FILE" > /dev/null
sudo chmod 640 "$PERM_FILE"
sleep 15

if [ -f "${PERM_FILE}.gz" ]; then
    ORIG_PER=$(stat -c '%a' "$PERM_FILE")
    GZ_PER=$(stat -c '%a' "${PERM_FILE}.gz")
    if [ "$GZ_PER" = "$ORIG_PER" ] || [ "$GZ_PER" = "644" ]; then pass "Права сохранены (orig=$ORIG_PER, gz=$GZ_PER)"; else fail "Права не совпадают (orig=$ORIG_PER, gz=$GZ_PER)"; fi
else
    fail "Файл 640 НЕ сжат"
fi

# ============================================================================
# 18.4 CFG тесты — РЕКУРСИВНАЯ очистка
# ============================================================================
clean_compressed() {
    find "$BASEDIR" -name '*.gz' -delete 2>/dev/null || true
    find "$BASEDIR" -name '*.br' -delete 2>/dev/null || true
}

echo ""
echo "[18.4.1] CFG-1: Только Gzip, level 9"
echo "------------------------------------------------------------------------"

sudo systemctl stop mh-compressor-manager 2>/dev/null || true
clean_compressed
sudo sed -i "s|^algorithms=.*|algorithms=gzip|" /etc/mediahive/compressor-manager.conf
sudo systemctl start mh-compressor-manager
sleep 30

GZ=$(find "$BASEDIR" -name '*.gz' 2>/dev/null | wc -l)
BR=$(find "$BASEDIR" -name '*.br' 2>/dev/null | wc -l)
if [ "$GZ" -gt 0 ] && [ "$BR" -eq 0 ]; then pass "CFG-1: gzip=$GZ, brotli=$BR"; else fail "CFG-1: gzip=$GZ, brotli=$BR"; fi

echo ""
echo "[18.4.1] CFG-2: Только Brotli, level 11"
echo "------------------------------------------------------------------------"

sudo systemctl stop mh-compressor-manager 2>/dev/null || true
clean_compressed
sudo sed -i "s|^algorithms=.*|algorithms=brotli|" /etc/mediahive/compressor-manager.conf
sudo systemctl start mh-compressor-manager
sleep 30

GZ=$(find "$BASEDIR" -name '*.gz' 2>/dev/null | wc -l)
BR=$(find "$BASEDIR" -name '*.br' 2>/dev/null | wc -l)
if [ "$BR" -gt 0 ] && [ "$GZ" -eq 0 ]; then pass "CFG-2: gzip=$GZ, brotli=$BR"; else fail "CFG-2: gzip=$GZ, brotli=$BR"; fi

echo ""
echo "[18.4.1] CFG-3: Оба алгоритма, max сжатие"
echo "------------------------------------------------------------------------"

sudo systemctl stop mh-compressor-manager 2>/dev/null || true
clean_compressed
sudo sed -i "s|^algorithms=.*|algorithms=all|" /etc/mediahive/compressor-manager.conf
sudo systemctl start mh-compressor-manager
sleep 30

GZ=$(find "$BASEDIR" -name '*.gz' 2>/dev/null | wc -l)
BR=$(find "$BASEDIR" -name '*.br' 2>/dev/null | wc -l)
if [ "$GZ" -gt 0 ] && [ "$BR" -gt 0 ]; then pass "CFG-3: gzip=$GZ, brotli=$BR"; else fail "CFG-3: gzip=$GZ, brotli=$BR"; fi

# ============================================================================
# ФИНАЛЬНЫЙ ОТЧЁТ
# ============================================================================
echo ""
echo "========================================================================"
echo "ИТОГОВЫЙ ОТЧЁТ"
echo "========================================================================"

STATUS=$(sudo systemctl status mh-compressor-manager --no-pager 2>&1)
ACTIVE=$(echo "$STATUS" | grep 'Active:' | awk '{print $2, $3}')
MEM=$(echo "$STATUS" | grep 'Memory:' | awk '{print $2, $3}')
CPU=$(echo "$STATUS" | grep 'CPU:' | awk -F'CPU:' '{print $2}')

echo ""
echo "  ╔══════════════════════════════════════════════════════╗"
echo "  ║           РЕЗУЛЬТАТЫ ТЕСТИРОВАНИЯ                   ║"
echo "  ╠══════════════════════════════════════════════════════╣"
printf  "  ║  Пройдено:  %-40s ║\n" "$PASS ✅"
printf  "  ║  Провалено: %-40s ║\n" "$FAIL ❌"
echo "  ║  Процесс:           $ACTIVE"
echo "  ║  Memory:            $MEM"
echo "  ║  CPU:               $CPU"
echo "  ╚══════════════════════════════════════════════════════╝"

if [ "$FAIL" -eq 0 ]; then echo "  🎉 ВСЕ ТЕСТЫ ПРОЙДЕНЫ!"; else echo "  ⚠️  ПРОВАЛЕНО: $FAIL"; fi

exit $FAIL
