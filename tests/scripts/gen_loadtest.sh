#!/bin/bash
set -e
BASEDIR=/srv/123/loadtest
echo "=== Генерация нагрузочных тестовых данных (1GB+) ==="

rm -rf "$BASEDIR"
mkdir -p "$BASEDIR"/{html,css,js,json,xml,txt}

# Быстрая генерация через dd + base64 вместо медленных bash-циклов
# 1. HTML - 80 файлов по ~100KB = 8MB
echo "Генерация HTML..."
for i in $(seq 1 80); do
  FILE="$BASEDIR/html/page_${i}.html"
  {
    echo '<!DOCTYPE html><html><head><title>Page</title></head><body>'
    for j in $(seq 1 100); do
      echo "<div class='article'><h2>Article $j</h2><p>Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.</p><img src='/images/photo_${i}_${j}.jpg'><footer>Published 2024-01-15 by Author $((RANDOM%10+1))</footer></div>"
    done
    echo '</body></html>'
  } > "$FILE"
done
echo "HTML: 80 файлов, размер: $(du -sh "$BASEDIR/html" | cut -f1)"

# 2. CSS - 50 файлов по ~150KB = 7.5MB
echo "Генерация CSS..."
for i in $(seq 1 50); do
  FILE="$BASEDIR/css/styles_${i}.css"
  {
    echo "/* Main stylesheet $i */"
    for j in $(seq 1 400); do
      echo ".class-${i}-${j} { margin: ${j}px; padding: ${j}px; color: #333; background: #fff; font-size: ${j}px; border: 1px solid #ccc; border-radius: 4px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }"
    done
  } > "$FILE"
done
echo "CSS: 50 файлов, размер: $(du -sh "$BASEDIR/css" | cut -f1)"

# 3. JS - 50 файлов по ~120KB = 6MB
echo "Генерация JS..."
for i in $(seq 1 50); do
  FILE="$BASEDIR/js/app_${i}.js"
  {
    echo "// Application module $i"
    echo "(function() {"
    for j in $(seq 1 250); do
      echo "var mod_${i}_${j} = { name: 'mod_$i_$j', version: '1.0.$j', init: function() { console.log('Init'); this.data = []; for(var k=0;k<50;k++){ this.data.push({id:k,value:'data_${i}_${j}_'+k}); } } };"
    done
    echo "})();"
  } > "$FILE"
done
echo "JS: 50 файлов, размер: $(du -sh "$BASEDIR/js" | cut -f1)"

# 4. JSON - 60 файлов по ~800KB = 48MB
echo "Генерация JSON..."
for i in $(seq 1 60); do
  FILE="$BASEDIR/json/data_${i}.json"
  {
    echo '{"records":['
    for j in $(seq 1 1000); do
      echo "{\"id\":$j,\"name\":\"record_${i}_${j}\",\"description\":\"Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation.\",\"tags\":[\"tag_${j}_a\",\"tag_${j}_b\",\"tag_${j}_c\"],\"metadata\":{\"created\":\"2024-01-15\",\"author\":\"user_$((RANDOM%100+1))\",\"priority\":$((RANDOM%5+1)),\"status\":\"active\",\"extra_data\":\"$(head -c 200 /dev/urandom | base64 | head -c 200)\"}}"
      if [ $j -lt 1000 ]; then echo -n ','; fi
    done
    echo ']}'
  } > "$FILE"
done
echo "JSON: 60 файлов, размер: $(du -sh "$BASEDIR/json" | cut -f1)"

# 5. XML - 50 файлов по ~400KB = 20MB
echo "Генерация XML..."
for i in $(seq 1 50); do
  FILE="$BASEDIR/xml/feed_${i}.xml"
  {
    echo '<?xml version="1.0"?><feed><title>Feed '$i'</title>'
    for j in $(seq 1 500); do
      echo "<entry id='$j' title='Entry $i-$j' author='user_$((RANDOM%50+1))'><content>Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. $(head -c 100 /dev/urandom | base64 | head -c 100)</content></entry>"
    done
    echo '</feed>'
  } > "$FILE"
done
echo "XML: 50 файлов, размер: $(du -sh "$BASEDIR/xml" | cut -f1)"

# 6. TXT (логи) - 80 файлов по ~22MB = 1.7GB — ГЕНЕРАЦИЯ ЧЕРЕЗ dd + base64
echo "Генерация TXT..."
for i in $(seq 1 80); do
  FILE="$BASEDIR/txt/log_${i}.txt"
  # Генерируем 20MB случайных данных, добавляем формат лога
  head -c 20000000 /dev/urandom | base64 | tr '\n' ' ' | fold -w 200 | head -n 80000 | \
    awk '{printf "2024-01-15 12:%02d:%02d [INFO] Request processed: method=GET path=/api/v1/resource/%d status=200 duration=%dms user_agent=\"Mozilla/5.0 (X11; Linux x86_64)\" remote_addr=192.168.%d.%d request_id=abc-%d-%s\n", int(rand()*60), int(rand()*60), NR, int(rand()*5000), int(rand()*256), int(rand()*256), FILENAME, substr($0,1,20)}' > "$FILE"
done
echo "TXT: 80 файлов, размер: $(du -sh "$BASEDIR/txt" | cut -f1)"

# Итог генерации
echo ""
echo "=== ИТОГ ГЕНЕРАЦИИ ==="
TOTAL=$(du -sh "$BASEDIR" | cut -f1)
COUNT=$(find "$BASEDIR" -type f | wc -l)
echo "Всего файлов: $COUNT"
echo "Общий размер: $TOTAL"

# Предварительное сжатие ~40% файлов
echo ""
echo "=== Предварительное сжатие ~40% файлов (gzip) ==="
COMPRESSED=0
TOTAL_TARGETS=$(find "$BASEDIR" -type f \( -name '*.html' -o -name '*.css' -o -name '*.js' -o -name '*.json' -o -name '*.xml' -o -name '*.txt' \) | wc -l)
TO_COMPRESS=$((TOTAL_TARGETS * 40 / 100))
echo "Будет сжато: $TO_COMPRESS из $TOTAL_TARGETS файлов"

IDX=0
for FILE in $(find "$BASEDIR" -type f \( -name '*.html' -o -name '*.css' -o -name '*.js' -o -name '*.json' -o -name '*.xml' -o -name '*.txt' \) | sort); do
  IDX=$((IDX + 1))
  if [ $IDX -le $TO_COMPRESS ]; then
    gzip -k -f "$FILE" 2>/dev/null || true
    touch -d "2024-01-01" "${FILE}.gz" 2>/dev/null || true
    COMPRESSED=$((COMPRESSED + 1))
  fi
done

echo "Предварительно сжато: $COMPRESSED файлов"
echo ""
echo "=== ФИНАЛЬНАЯ СТАТИСТИКА ==="
echo "Всего целевых файлов: $TOTAL_TARGETS"
echo "Сжато заранее: $COMPRESSED ($(echo "scale=1; $COMPRESSED * 100 / $TOTAL_TARGETS" | bc)%)"
echo "Не сжато: $((TOTAL_TARGETS - COMPRESSED))"
echo "Общий размер директории: $(du -sh "$BASEDIR" | cut -f1)"
