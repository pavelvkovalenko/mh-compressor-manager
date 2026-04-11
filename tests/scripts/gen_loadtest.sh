#!/bin/bash
set -e
BASEDIR=/srv/123/loadtest
echo "=== Генерация нагрузочных тестовых данных (1GB+) ==="

rm -rf "$BASEDIR"
mkdir -p "$BASEDIR"/{html,css,js,json,xml,txt}

# 1. HTML файлы - 80 штук (~9MB)
echo "Генерация HTML..."
for i in $(seq 1 80); do
  FILE="$BASEDIR/html/page_${i}.html"
  {
    echo "<!DOCTYPE html><html><head><title>Page $i</title><meta charset=\"utf-8\"></head><body>"
    for j in $(seq 1 300); do
      echo "<div class=\"article article-$j\"><h2>Article Title $j</h2>"
      echo "<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.</p>"
      echo "<img src=\"/images/photo_$i_$j.jpg\" alt=\"Photo $j\"><br>"
      echo "<footer>Published on 2024-01-15 by Author $((RANDOM%10+1))</footer></div>"
    done
    echo "</body></html>"
  } > "$FILE"
done
echo "HTML: 80 файлов, размер: $(du -sh "$BASEDIR/html" | cut -f1)"

# 2. CSS файлы - 50 штук (~7MB)
echo "Генерация CSS..."
for i in $(seq 1 50); do
  FILE="$BASEDIR/css/styles_${i}.css"
  {
    echo "/* Main stylesheet $i */"
    for j in $(seq 1 800); do
      echo ".class-${i}-${j} { margin: ${j}px; padding: ${j}px; color: #333; background-color: #fff; font-size: ${j}px; border: 1px solid #ccc; border-radius: 4px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }"
    done
  } > "$FILE"
done
echo "CSS: 50 файлов, размер: $(du -sh "$BASEDIR/css" | cut -f1)"

# 3. JS файлы - 50 штук (~6MB)
echo "Генерация JS..."
for i in $(seq 1 50); do
  FILE="$BASEDIR/js/app_${i}.js"
  {
    echo "// Application module $i"
    echo "(function() {"
    for j in $(seq 1 500); do
      echo "var module_${i}_${j} = { name: \"module_$i_$j\", version: \"1.0.$j\", init: function() { console.log(\"Module initialized\"); this.data = []; for(var k=0;k<100;k++){ this.data.push({id:k,value:\"data_$i_${j}_\"+k,extra:\"additional_data_for_testing_$k\"}); } } };"
    done
    echo "})();"
  } > "$FILE"
done
echo "JS: 50 файлов, размер: $(du -sh "$BASEDIR/js" | cut -f1)"

# 4. JSON файлы - 60 штук (~49MB)
echo "Генерация JSON..."
for i in $(seq 1 60); do
  FILE="$BASEDIR/json/api_data_${i}.json"
  {
    echo '{"api_version":"2.0","data":['
    for j in $(seq 1 3000); do
      echo "{\"id\":$j,\"title\":\"Item $i.$j\",\"description\":\"This is a detailed description for item $j in dataset $i with metadata for testing compression algorithms and performance.\",\"category\":\"category_$((RANDOM%50+1))\",\"tags\":[\"tag1\",\"tag2\",\"tag3\"],\"created_at\":\"2024-01-15T12:00:00Z\",\"status\":\"active\"},"
    done
    echo '{"id":9999,"title":"last"}]}'
  } > "$FILE"
done
echo "JSON: 60 файлов, размер: $(du -sh "$BASEDIR/json" | cut -f1)"

# 5. XML файлы - 50 штук (~20MB)
echo "Генерация XML..."
for i in $(seq 1 50); do
  FILE="$BASEDIR/xml/feed_${i}.xml"
  {
    echo "<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>Feed $i</title>"
    for j in $(seq 1 1500); do
      echo "<item><title>Article $i.$j</title><link>https://example.com/article/$i/$j</link><description>A detailed description of article $j in feed $i with lots of text for compression testing purposes.</description><pubDate>Mon, 15 Jan 2024 12:00:00 +0000</pubDate></item>"
    done
    echo "</channel></rss>"
  } > "$FILE"
done
echo "XML: 50 файлов, размер: $(du -sh "$BASEDIR/xml" | cut -f1)"

# 6. TXT файлы - логи - 80 штук (~1.7GB)
echo "Генерация TXT..."
for i in $(seq 1 80); do
  FILE="$BASEDIR/txt/log_${i}.txt"
  for j in $(seq 1 80000); do
    echo "2024-01-15 12:$((RANDOM%60)):$((RANDOM%60)) [INFO] Request processed: method=GET path=/api/v1/resource/$j status=200 duration=${RANDOM}ms user_agent=\"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\" remote_addr=192.168.$((RANDOM%256)).$((RANDOM%256)) request_id=abc-$i-$j"
  done > "$FILE"
done
echo "TXT: 80 файлов, размер: $(du -sh "$BASEDIR/txt" | cut -f1)"

# Итог генерации
echo ""
echo "=== ИТОГ ГЕНЕРАЦИИ ==="
TOTAL=$(du -sh "$BASEDIR" | cut -f1)
COUNT=$(find "$BASEDIR" -type f | wc -l)
echo "Всего файлов: $COUNT"
echo "Общий размер: $TOTAL"

# Предварительное сжатие ~40% файлов (для проверки stale-механизма)
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
    # Устаревший файл - на 1 день старше источника
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
