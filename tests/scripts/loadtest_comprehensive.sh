#!/bin/bash
# ============================================================================
# Комплексное нагрузочное тестирование mh-compressor-manager
# Тесты: CPU нагрузка, память, стабильность, корректность сжатия
# ============================================================================
set -e

BASEDIR=/srv/123/loadtest
LOGFILE=/tmp/loadtest_results.txt

exec > >(tee "$LOGFILE") 2>&1

echo "========================================================================"
echo "НАГРУЗОЧНОЕ ТЕСТИРОВАНИЕ mh-compressor-manager"
echo "Дата: $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================================================"

# ---------------------------------------------------------------------------
# ЭТАП 0: Подготовка
# ---------------------------------------------------------------------------
echo ""
echo "[ЭТАП 0] Подготовка среды"
echo "------------------------------------------------------------------------"

sudo systemctl stop mh-compressor-manager 2>/dev/null || true
sudo rm -rf "$BASEDIR"
sudo mkdir -p "$BASEDIR"/{html,css,js,json,txt}

echo "✓ Сервис остановлен, директория очищена"

# ---------------------------------------------------------------------------
# ЭТАП 1: Генерация данных (~500MB, 500 файлов)
# ---------------------------------------------------------------------------
echo ""
echo "[ЭТАП 1] Генерация тестовых данных (~500MB, 500 файлов)"
echo "------------------------------------------------------------------------"

START_GEN=$(date +%s)

# HTML: 100 файлов по ~500KB = 50MB
echo "  Генерация HTML (100 файлов)..."
for i in $(seq 1 100); do
  {
    echo "<!DOCTYPE html><html><head><title>Page $i</title></head><body>"
    for j in $(seq 1 500); do
      echo "<p class='item-$j'>Lorem ipsum dolor sit amet, consectetur adipiscing elit. Section $i, item $j.</p>"
    done
    echo "</body></html>"
  } > "$BASEDIR/html/page_${i}.html"
done
echo "  ✓ HTML: $(du -sh "$BASEDIR/html" | cut -f1)"

# CSS: 80 файлов по ~200KB = 16MB
echo "  Генерация CSS (80 файлов)..."
for i in $(seq 1 80); do
  {
    echo "/* Stylesheet $i */"
    for j in $(seq 1 300); do
      echo ".class-${i}-${j} { margin: ${j}px; padding: ${j}px; color: #333; font-size: ${j}px; border: 1px solid #ccc; }"
    done
  } > "$BASEDIR/css/styles_${i}.css"
done
echo "  ✓ CSS: $(du -sh "$BASEDIR/css" | cut -f1)"

# JS: 80 файлов по ~300KB = 24MB
echo "  Генерация JS (80 файлов)..."
for i in $(seq 1 80); do
  {
    echo "// Module $i"
    echo "(function(){"
    for j in $(seq 1 400); do
      echo "var m${i}_${j}={name:'m${i}_${j}',init:function(){this.data=[];for(var k=0;k<50;k++)this.data.push({id:k,val:'d${i}_${j}_${k}'});}}};"
    done
    echo "})();"
  } > "$BASEDIR/js/app_${i}.js"
done
echo "  ✓ JS: $(du -sh "$BASEDIR/js" | cut -f1)"

# JSON: 120 файлов по ~1MB = 120MB
echo "  Генерация JSON (120 файлов)..."
for i in $(seq 1 120); do
  {
    echo '{"api":"2.0","data":['
    for j in $(seq 1 1000); do
      echo "{\"id\":$j,\"title\":\"Item $i.$j\",\"desc\":\"Description for item $j in dataset $i with metadata.\",\"cat\":\"c$((RANDOM%20+1))\",\"status\":\"active\"},"
    done
    echo '{"id":9999,"title":"last"}]}'
  } > "$BASEDIR/json/data_${i}.json"
done
echo "  ✓ JSON: $(du -sh "$BASEDIR/json" | cut -f1)"

# TXT (логи): 120 файлов по ~3MB = 360MB
echo "  Генерация TXT логов (120 файлов)..."
for i in $(seq 1 120); do
  {
    for j in $(seq 1 30000); do
      echo "2024-06-15 12:$((RANDOM%60)):$((RANDOM%60)) [INFO] GET /api/v1/item/$j 200 ${RANDOM}ms ua=\"Mozilla/5.0\" ip=10.0.$((RANDOM%256)).$((RANDOM%256))"
    done
  } > "$BASEDIR/txt/access_${i}.log.txt"
done
echo "  ✓ TXT: $(du -sh "$BASEDIR/txt" | cut -f1)"

END_GEN=$(date +%s)
TOTAL_FILES=$(find "$BASEDIR" -type f | wc -l)
TOTAL_SIZE=$(du -sh "$BASEDIR" | cut -f1)

echo ""
echo "  Файлов сгенерировано: $TOTAL_FILES"
echo "  Общий размер: $TOTAL_SIZE"
echo "  Время генерации: $((END_GEN - START_GEN)) сек"

# ---------------------------------------------------------------------------
# ЭТАП 2: Предварительное сжатие 40% (для теста stale detection)
# ---------------------------------------------------------------------------
echo ""
echo "[ЭТАП 2] Предварительное сжатие 40% файлов (stale detection)"
echo "------------------------------------------------------------------------"

TO_COMPRESS=$((TOTAL_FILES * 40 / 100))
echo "  Будет сжато: $TO_COMPRESS из $TOTAL_FILES"

IDX=0
for FILE in $(find "$BASEDIR" -type f \( -name '*.html' -o -name '*.css' -o -name '*.js' -o -name '*.json' -o -name '*.txt' \) | sort | head -n $TO_COMPRESS); do
  gzip -k -f "$FILE" 2>/dev/null || true
  # Делаем сжатый файл СТАРШЕ источника
  touch -d "2024-01-01" "${FILE}.gz" 2>/dev/null || true
  IDX=$((IDX + 1))
done
echo "  ✓ Сжато заранее: $IDX файлов"

# ---------------------------------------------------------------------------
# ЭТАП 3: Нагрузочный тест — запуск и обработка
# ---------------------------------------------------------------------------
echo ""
echo "[ЭТАП 3] Запуск сервиса и нагрузочное тестирование"
echo "------------------------------------------------------------------------"

# Настраиваем конфиг на loadtest
sudo sed -i "s|^target_path=.*|target_path=$BASEDIR|" /etc/mediahive/compressor-manager.conf

# Запоминаем стартовые метрики
MEM_BEFORE=$(free -m | awk '/^Mem:/{print $3}')

START_TIME=$(date +%s)
sudo systemctl start mh-compressor-manager
echo "  Сервис запущен в $(date '+%H:%M:%S')"

# Мониторинг каждые 30 секунд
for CHECK in 30 60 120 180; do
  echo ""
  echo "  --- Через ${CHECK} сек ---"
  sleep $((CHECK - 30))

  COMPRESSED=$(find "$BASEDIR" -name '*.gz' -o -name '*.br' 2>/dev/null | wc -l)
  UNCOMPRESSED=$(find "$BASEDIR" -type f \( -name '*.html' -o -name '*.css' -o -name '*.js' -o -name '*.json' -o -name '*.txt' \) ! -name '*.gz' ! -name '*.br' 2>/dev/null | wc -l)

  MEM=$(sudo systemctl status mh-compressor-manager --no-pager 2>&1 | grep 'Memory:' | awk '{print $2, $3}')
  CPU=$(sudo systemctl status mh-compressor-manager --no-pager 2>&1 | grep 'CPU:' | awk -F'CPU:' '{print $2}')

  echo "  Сжатых: $COMPRESSED | Несжатых: $UNCOMPRESSED | Memory: $MEM | CPU: $CPU"
done

END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

# ---------------------------------------------------------------------------
# ЭТАП 4: Проверка корректности сжатия
# ---------------------------------------------------------------------------
echo ""
echo "[ЭТАП 4] Проверка корректности сжатия"
echo "------------------------------------------------------------------------"

ERRORS=0
VALID_GZ=0
VALID_BR=0
INVALID_GZ=0
INVALID_BR=0
SIZE_MISMATCH=0

# Проверяем случайные 20 сжатых файлов
for GZ in $(find "$BASEDIR" -name '*.html.gz' -o -name '*.json.gz' 2>/dev/null | shuf | head -10); do
  ORIG="${GZ%.gz}"
  if [ -f "$ORIG" ]; then
    DECOMP=$(gunzip -c "$GZ" 2>/dev/null | wc -c)
    ORIG_SIZE=$(wc -c < "$ORIG")
    GZ_SIZE=$(wc -c < "$GZ")
    if [ "$DECOMP" -eq "$ORIG_SIZE" ] 2>/dev/null; then
      VALID_GZ=$((VALID_GZ + 1))
    else
      INVALID_GZ=$((INVALID_GZ + 1))
      SIZE_MISMATCH=$((SIZE_MISMATCH + 1))
      echo "  ❌ gzip mismatch: $ORIG (orig=$ORIG_SIZE, decomp=$DECOMP, gz=$GZ_SIZE)"
    fi
  fi
done

for BR in $(find "$BASEDIR" -name '*.html.br' -o -name '*.json.br' 2>/dev/null | shuf | head -10); do
  ORIG="${BR%.br}"
  if [ -f "$ORIG" ]; then
    DECOMP=$(brotli -d -c "$BR" 2>/dev/null | wc -c)
    ORIG_SIZE=$(wc -c < "$ORIG")
    BR_SIZE=$(wc -c < "$BR")
    if [ "$DECOMP" -eq "$ORIG_SIZE" ] 2>/dev/null; then
      VALID_BR=$((VALID_BR + 1))
    else
      INVALID_BR=$((INVALID_BR + 1))
      SIZE_MISMATCH=$((SIZE_MISMATCH + 1))
      echo "  ❌ brotli mismatch: $ORIG (orig=$ORIG_SIZE, decomp=$DECOMP, br=$BR_SIZE)"
    fi
  fi
done

echo "  gzip: валидных=$VALID_GZ, невалидных=$INVALID_GZ"
echo "  brotli: валидных=$VALID_BR, невалидных=$INVALID_BR"
echo "  Размерных несоответствий: $SIZE_MISMATCH"

# ---------------------------------------------------------------------------
# ЭТАП 5: Стресс-тест — добавление 100 новых файлов
# ---------------------------------------------------------------------------
echo ""
echo "[ЭТАП 5] Стресс-тест — 100 новых файлов за раз"
echo "------------------------------------------------------------------------"

STRESS_START=$(date +%s)
for i in $(seq 1 100); do
  {
    echo "<html><body><h1>Stress $i</h1>"
    for j in $(seq 1 200); do echo "<p>Content paragraph $j for stress test file $i.</p>"; done
    echo "</body></html>"
  } > "$BASEDIR/html/stress_${i}.html"
done
echo "  100 файлов созданы за $(( $(date +%s) - STRESS_START )) сек"

echo "  Ждём обработку (15 сек)..."
sleep 15

STRESS_COMPRESSED=$(find "$BASEDIR/html" -name 'stress_*.html.gz' -o -name 'stress_*.html.br' 2>/dev/null | wc -l)
echo "  Сжатых stress-файлов: $STRESS_COMPRESSED / 200 (ожидаемо ~200 если >= 1024b)"

# ---------------------------------------------------------------------------
# ЭТАП 6: Финальные метрики
# ---------------------------------------------------------------------------
echo ""
echo "[ЭТАП 6] Финальные метрики"
echo "------------------------------------------------------------------------"

FINAL_COMPRESSED=$(find "$BASEDIR" -name '*.gz' -o -name '*.br' 2>/dev/null | wc -l)
FINAL_UNCOMPRESSED=$(find "$BASEDIR" -type f \( -name '*.html' -o -name '*.css' -o -name '*.js' -o -name '*.json' -o -name '*.txt' \) ! -name '*.gz' ! -name '*.br' 2>/dev/null | wc -l)

STATUS=$(sudo systemctl status mh-compressor-manager --no-pager 2>&1)
MEM_FINAL=$(echo "$STATUS" | grep 'Memory:' | awk '{print $2, $3}')
CPU_FINAL=$(echo "$STATUS" | grep 'CPU:' | awk -F'CPU:' '{print $2}')
ACTIVE=$(echo "$STATUS" | grep 'Active:' | awk '{print $2, $3}')

MEM_AFTER=$(free -m | awk '/^Mem:/{print $3}')
SYS_MEM_DELTA=$((MEM_AFTER - MEM_BEFORE))

ERROR_COUNT=$(journalctl -u mh-compressor-manager --since "$DURATION seconds ago" --no-pager 2>&1 | grep -ciE 'error|fail|panic|Bad address' || echo 0)

echo ""
echo "  ╔══════════════════════════════════════════════════════════╗"
echo "  ║              ИТОГОВЫЕ РЕЗУЛЬТАТЫ                        ║"
echo "  ╠══════════════════════════════════════════════════════════╣"
echo "  ║  Длительность теста:     ${DURATION} сек                      ║"
echo "  ║  Файлов сгенерировано:   $TOTAL_FILES                      ║"
echo "  ║  Объём данных:          $TOTAL_SIZE                         ║"
echo "  ║  Сжатых файлов:         $FINAL_COMPRESSED                      ║"
echo "  ║  Несжатых файлов:       $FINAL_UNCOMPRESSED                      ║"
echo "  ║  Процесс:               $ACTIVE     ║"
echo "  ║  Memory сервиса:        $MEM_FINAL                     ║"
echo "  ║  CPU сервиса:           $CPU_FINAL   ║"
echo "  ║  Системная память Δ:    ${SYS_MEM_DELTA}MB                      ║"
echo "  ║  gzip валидных:         $VALID_GZ / $((VALID_GZ + INVALID_GZ))                     ║"
echo "  ║  brotli валидных:       $VALID_BR / $((VALID_BR + INVALID_BR))                     ║"
echo "  ║  Ошибок в логах:        $ERROR_COUNT                      ║"
echo "  ╚══════════════════════════════════════════════════════════╝"

# ---------------------------------------------------------------------------
# ЭТАП 7: Очистка
# ---------------------------------------------------------------------------
echo ""
echo "[ЭТАП 7] Очистка"
echo "------------------------------------------------------------------------"

# Восстанавливаем конфиг
sudo sed -i 's|^target_path=.*|target_path=/srv/123|' /etc/mediahive/compressor-manager.conf
sudo rm -rf "$BASEDIR"
echo "  ✓ Директория удалена, конфиг восстановлен"

echo ""
echo "========================================================================"
echo "ТЕСТИРОВАНИЕ ЗАВЕРШЕНО"
echo "========================================================================"
