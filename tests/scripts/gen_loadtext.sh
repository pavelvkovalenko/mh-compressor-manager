#!/bin/bash
set -euo pipefail

BASEDIR=/srv/123/loadtest
echo "=== Генерация нагрузочных тестовых данных (1GB+) ==="
rm -rf "$BASEDIR"
mkdir -p "$BASEDIR"

# Генератор реалистичного текстового контента
# Повторяющиеся строки обеспечивают высокий коэффициент сжатия (60-80%)

gen_html() {
    local count=$1
    local line='<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.</p>'
    local header='<!DOCTYPE html><html><head><title>Test Page</title></head><body>'
    local footer='</body></html>'
    for i in $(seq 1 $count); do
        echo "$header"
        for j in $(seq 1 200); do echo "$line"; done
        echo "$footer"
    done
}

gen_css() {
    local count=$1
    local rule='.class { margin: 0; padding: 0; box-sizing: border-box; display: flex; align-items: center; justify-content: center; font-family: sans-serif; color: #333; background-color: #fff; border: 1px solid #ccc; border-radius: 4px; }'
    for i in $(seq 1 $count); do
        echo "/* Stylesheet $i */"
        for j in $(seq 1 200); do echo ".selector-${j} { $rule }"; done
    done
}

gen_js() {
    local count=$1
    local func='function testFunc() { var x = 0; for (var i = 0; i < 100; i++) { x += i; } return x; }'
    for i in $(seq 1 $count); do
        echo "// JavaScript module $i"
        for j in $(seq 1 200); do echo "var module${j} = { $func };"; done
    done
}

gen_json() {
    local count=$1
    for i in $(seq 1 $count); do
        echo "{"
        echo "  \"id\": $i,"
        echo "  \"name\": \"Test Entry Number $i\","
        echo "  \"description\": \"This is a test entry for load testing purposes with repeated text data to ensure good compression ratios for JSON files.\","
        for j in $(seq 1 50); do
            echo "  \"field_${j}\": \"Value ${j} for entry ${i}\","
        done
        echo "  \"timestamp\": \"2026-04-14T12:00:00Z\""
        echo "}"
    done
}

gen_txt() {
    local lines=$1
    local line='2026-04-14 12:00:00 [INFO] Request processed: GET /api/data?id=12345&format=json status=200 duration=42ms client=192.168.1.100 user_agent="Mozilla/5.0 (Windows NT 10.0; Win64; x64)"'
    for i in $(seq 1 $lines); do echo "$line"; done
}

gen_xml() {
    local count=$1
    for i in $(seq 1 $count); do
        echo "<?xml version=\"1.0\" encoding=\"UTF-8\"?><root>"
        for j in $(seq 1 100); do
            echo "<entry id=\"${j}\" name=\"Test Entry ${j}\" description=\"This is a test entry for load testing with repeated XML data\"><value>${j}</value><status>active</status><timestamp>2026-04-14T12:00:00Z</timestamp></entry>"
        done
        echo "</root>"
    done
}

echo "Генерация HTML..."
gen_html 50 > "$BASEDIR/pages.html"
echo "  HTML: $(du -sh "$BASEDIR/pages.html" | cut -f1)"

echo "Генерация CSS..."
gen_css 30 > "$BASEDIR/styles.css"
echo "  CSS: $(du -sh "$BASEDIR/styles.css" | cut -f1)"

echo "Генерация JS..."
gen_js 30 > "$BASEDIR/app.js"
echo "  JS: $(du -sh "$BASEDIR/app.js" | cut -f1)"

echo "Генерация JSON..."
gen_json 40 > "$BASEDIR/data.json"
echo "  JSON: $(du -sh "$BASEDIR/data.json" | cut -f1)"

echo "Генерация XML..."
gen_xml 40 > "$BASEDIR/feed.xml"
echo "  XML: $(du -sh "$BASEDIR/feed.xml" | cut -f1)"

echo "Генерация TXT (логи)..."
gen_txt 5000000 > "$BASEDIR/access.log.txt"
echo "  TXT: $(du -sh "$BASEDIR/access.log.txt" | cut -f1)"

# Разбиваем большие файлы на части для реалистичности
echo "Разбиение на отдельные файлы..."
split -l 100000 "$BASEDIR/access.log.txt" "$BASEDIR/log_"
rm -f "$BASEDIR/access.log.txt"
for f in "$BASEDIR"/log_*; do mv "$f" "${f}.txt"; done

split -l 50 "$BASEDIR/pages.html" "$BASEDIR/page_"
rm -f "$BASEDIR/pages.html"
for f in "$BASEDIR"/page_*; do mv "$f" "${f}.html"; done

split -l 30 "$BASEDIR/styles.css" "$BASEDIR/style_"
rm -f "$BASEDIR/styles.css"
for f in "$BASEDIR"/style_*; do mv "$f" "${f}.css"; done

split -l 30 "$BASEDIR/app.js" "$BASEDIR/module_"
rm -f "$BASEDIR/app.js"
for f in "$BASEDIR"/module_*; do mv "$f" "${f}.js"; done

split -l 5 "$BASEDIR/data.json" "$BASEDIR/data_"
rm -f "$BASEDIR/data.json"
for f in "$BASEDIR"/data_*; do mv "$f" "${f}.json"; done

split -l 5 "$BASEDIR/feed.xml" "$BASEDIR/feed_"
rm -f "$BASEDIR/feed.xml"
for f in "$BASEDIR"/feed_*; do mv "$f" "${f}.xml"; done

echo "=== ИТОГ ==="
echo "Всего файлов: $(find "$BASEDIR" -type f | wc -l)"
echo "Общий размер: $(du -sh "$BASEDIR" | cut -f1)"
