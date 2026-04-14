#!/bin/bash
set -eu
BASEDIR="/srv/123/loadtest"
SERVICE="mh-compressor-manager"

echo "=== Остановка сервиса ==="
sudo systemctl stop "$SERVICE" 2>/dev/null || true
sleep 1
sudo rm -rf "$BASEDIR"
sudo mkdir -p "$BASEDIR"

echo "=== Генерация 100 файлов JS (500KB-2MB, текстовый контент) ==="
# Генерируем реалистичный JS-контент который хорошо сжимается
JS_CONTENT='var app = { name: "TestApp", version: "1.0.0", config: { debug: false, logLevel: "info", maxRetries: 3, timeout: 5000 }, init: function() { console.log("App initialized"); this.loadData(); this.bindEvents(); }, loadData: function() { fetch("/api/data").then(r => r.json()).then(d => { this.data = d; this.render(); }); }, bindEvents: function() { document.addEventListener("click", (e) => { if (e.target.matches(".btn")) { this.handleClick(e); } }); }, handleClick: function(e) { console.log("Button clicked:", e.target.id); }, render: function() { const el = document.getElementById("root"); if (el) { el.innerHTML = "<h1>" + this.data.title + "</h1><p>" + this.data.description + "</p>"; } } }; document.addEventListener("DOMContentLoaded", () => app.init());'

for i in $(seq 1 100); do
    size=$((500000 + RANDOM % 1500000))
    # Повторяем контент нужное количество раз для достижения размера
    repeats=$((size / ${#JS_CONTENT} + 1))
    for r in $(seq 1 $repeats); do echo "$JS_CONTENT"; done | head -c "$size" > "$BASEDIR/app_${i}.js"
done
echo "Создано $(ls "$BASEDIR" | wc -l) файлов, размер: $(du -sh "$BASEDIR" | cut -f1)"

echo "=== Запуск сервиса ==="
START_TIME=$(date +%s)
sudo systemctl start "$SERVICE"

echo "=== Ожидание 30 секунд ==="
for i in $(seq 1 6); do
    sleep 5
    gz=$(sudo find "$BASEDIR" -name "*.gz" 2>/dev/null | wc -l)
    br=$(sudo find "$BASEDIR" -name "*.br" 2>/dev/null | wc -l)
    mem=$(sudo systemctl status "$SERVICE" --no-pager 2>/dev/null | grep Memory | head -1 || echo "N/A")
    echo "  Прогресс (${i}x5 сек): gzip=$gz, brotli=$br, $mem"
done

ELAPSED=$(($(date +%s) - START_TIME))
echo "=== Проверка результатов (время: ${ELAPSED} сек) ==="
total_js=$(sudo find "$BASEDIR" -name "*.js" ! -name "*.gz" ! -name "*.br" | wc -l)
total_gz=$(sudo find "$BASEDIR" -name "*.js.gz" | wc -l)
total_br=$(sudo find "$BASEDIR" -name "*.js.br" | wc -l)

echo "Исходные JS: $total_js"
echo "Gzip: $total_gz"
echo "Brotli: $total_br"

# Валидация
valid_gz=0
for f in $(sudo find "$BASEDIR" -name "*.js.gz" | head -10); do
    if sudo gzip -t "$f" 2>/dev/null; then
        valid_gz=$((valid_gz + 1))
    fi
done
echo "Валидация gzip: $valid_gz/10"

# Память
echo "=== Память сервиса ==="
sudo systemctl status "$SERVICE" --no-pager 2>/dev/null | grep -E "Memory|Tasks|Status"

echo "=== Логи (последние 15 строк) ==="
sudo journalctl -u "$SERVICE" --no-pager -n 15 2>/dev/null | tail -15

sudo systemctl stop "$SERVICE"
echo "=== Тест завершён ==="
