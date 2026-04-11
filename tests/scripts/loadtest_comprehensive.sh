#!/bin/bash
set -e
BASEDIR=/srv/123/loadtest
echo "=== Генерация нагрузочных тестовых данных (1GB+) ==="
rm -rf "$BASEDIR"
mkdir -p "$BASEDIR"

echo "Генерация данных через dd..."

# HTML - 8 файлов по 1MB = 8MB
dd if=/dev/urandom bs=1M count=8 2>/dev/null | split -b 1M - "$BASEDIR/html_page_"
for f in "$BASEDIR"/html_page_*; do mv "$f" "${f}.html"; done

# CSS - 7 файлов по 1MB = 7MB
dd if=/dev/urandom bs=1M count=7 2>/dev/null | split -b 1M - "$BASEDIR/css_style_"
for f in "$BASEDIR"/css_style_*; do mv "$f" "${f}.css"; done

# JS - 6 файлов по 1MB = 6MB
dd if=/dev/urandom bs=1M count=6 2>/dev/null | split -b 1M - "$BASEDIR/js_app_"
for f in "$BASEDIR"/js_app_*; do mv "$f" "${f}.js"; done

# TXT (логи) - 80 файлов по ~22MB = 1.7GB
dd if=/dev/urandom bs=1M count=1700 2>/dev/null | split -b 22M - "$BASEDIR/log_"
for f in "$BASEDIR"/log_*; do mv "$f" "${f}.txt"; done

echo "=== ИТОГ ==="
echo "Всего файлов: $(find "$BASEDIR" -type f | wc -l)"
echo "Общий размер: $(du -sh "$BASEDIR" | cut -f1)"
