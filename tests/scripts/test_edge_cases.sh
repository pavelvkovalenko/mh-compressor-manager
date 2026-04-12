#!/bin/bash
# =============================================================================
# test_edge_cases.sh — Тестирование граничных условий (ТЗ §18.6.2)
# Проверяет EDGE-1 через EDGE-12
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/edge_test"
SERVICE="mh-compressor-manager"
PASS=0
FAIL=0
TOTAL=0

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }
check() {
    TOTAL=$((TOTAL + 1))
    if eval "$1"; then
        PASS=$((PASS + 1))
        echo "  ✅ PASS [$TOTAL]: $2"
    else
        FAIL=$((FAIL + 1))
        echo "  ❌ FAIL [$TOTAL]: $2"
    fi
}

cleanup() {
    sudo systemctl stop "$SERVICE" 2>/dev/null || true
    sudo rm -rf "$BASEDIR"
}
trap cleanup EXIT

# ===========================================================================
log "=== Подготовка ==="
cleanup
sudo mkdir -p "$BASEDIR"

# ===========================================================================
log "=== EDGE-1: Пустая директория ==="
sudo systemctl start "$SERVICE"
sleep 3

STATUS=$(sudo systemctl is-active "$SERVICE")
check "[ '$STATUS' = 'active' ]" "EDGE-1: Сервис запущен с пустой директорией"

# ===========================================================================
log "=== EDGE-2: Директория не существует ==="
sudo systemctl stop "$SERVICE"
# Тест через dry-run с несуществующей директорией
sudo mh-compressor-manager --dir /nonexistent/path --dry-run 2>&1 | grep -qi "error\|fail\|не найдено" && EDGE2_OK=true || EDGE2_OK=false
check "$EDGE2_OK" "EDGE-2: Ошибка при несуществующей директории"

# ===========================================================================
log "=== EDGE-3: Файл ровно 256 байт ==="
sudo mkdir -p "$BASEDIR"
python3 -c "import sys; sys.stdout.buffer.write(b'A' * 256)" > "$BASEDIR/exactly_256.js"
sleep 3

check "[ -f '$BASEDIR/exactly_256.js.gz' ]" "EDGE-3a: файл 256 байт сжат (.gz)"
check "[ -f '$BASEDIR/exactly_256.js.br' ]" "EDGE-3b: файл 256 байт сжат (.br)"

# ===========================================================================
log "=== EDGE-4: Файл ровно 255 байт ==="
python3 -c "import sys; sys.stdout.buffer.write(b'B' * 255)" > "$BASEDIR/exactly_255.js"
sleep 3

check "[ ! -f '$BASEDIR/exactly_255.js.gz' ]" "EDGE-4a: файл 255 байт НЕ сжат (.gz)"
check "[ ! -f '$BASEDIR/exactly_255.js.br' ]" "EDGE-4b: файл 255 байт НЕ сжат (.br)"

# ===========================================================================
log "=== EDGE-7: Файл с пробелами и спецсимволами в имени ==="
python3 -c "print('test')" > "$BASEDIR/file with spaces & (special).js"
sleep 3

check "[ -f '$BASEDIR/file with spaces & (special).js.gz' ]" "EDGE-7a: файл с пробелами сжат"

# ===========================================================================
log "=== EDGE-8: 100 одновременных изменений ==="
STRESS_START=$(date +%s)
for i in $(seq 1 100); do
    python3 -c "print('Stress $i ' + 'X' * 500)" > "$BASEDIR/stress_${i}.txt" &
done
wait
STRESS_ELAPSED=$(($(date +%s) - STRESS_START))
log "  100 файлов созданы за ${STRESS_ELAPSED}с, ожидание обработки..."

sleep 10
PROCESSED=$(sudo find "$BASEDIR" -name "stress_*.txt.gz" | wc -l)
check "[ $PROCESSED -ge 95 ]" "EDGE-8: $PROCESSED/100 файлов обработаны после batch create"

# ===========================================================================
log "=== EDGE-9: Файл был 1025 байт (сжат), стал 171 байт ==="
python3 -c "print('A' * 1024)" > "$BASEDIR/shrink_test.txt"
sleep 5

check "[ -f '$BASEDIR/shrink_test.txt.gz' ]" "EDGE-9a: .gz создана"

# Уменьшаем
python3 -c "print('B' * 170)" > "$BASEDIR/shrink_test.txt"
sleep 5

check "[ ! -f '$BASEDIR/shrink_test.txt.gz' ]" "EDGE-9b: stale .gz удалена после уменьшения"
check "[ ! -f '$BASEDIR/shrink_test.txt.br' ]" "EDGE-9c: stale .br не создана"

# ===========================================================================
log "=== EDGE-10: Файл был 171 байт, стал 1025 байт ==="
python3 -c "print('C' * 170)" > "$BASEDIR/grow_test.txt"
sleep 3

check "[ ! -f '$BASEDIR/grow_test.txt.gz' ]" "EDGE-10a: при 171 байт .gz не создана"

# Увеличиваем
python3 -c "print('D' * 1024)" > "$BASEDIR/grow_test.txt"
sleep 5

check "[ -f '$BASEDIR/grow_test.txt.gz' ]" "EDGE-10b: при 1025 байт .gz создана"

# ===========================================================================
log "=== EDGE-11: Stale с совпадающим mtime, но размер < порога ==="
python3 -c "print('E' * 500)" > "$BASEDIR/stale_mtime.txt"
sleep 5

# Сохраняем mtime оригинала
ORIG_MTIME=$(stat -c %Y "$BASEDIR/stale_mtime.txt")

# Заменяем файлом 100 байт с тем же mtime
python3 -c "print('F' * 99)" > "$BASEDIR/stale_mtime.txt"
touch -d "@$ORIG_MTIME" "$BASEDIR/stale_mtime.txt" 2>/dev/null || true
sleep 5

check "[ ! -f '$BASEDIR/stale_mtime.txt.gz' ]" "EDGE-11: stale .gz удалена несмотря на совпадение mtime"

# ===========================================================================
log "=== EDGE-12: Файл колеблется: 300 → 150 → 400 → 100 байт ==="
# Шаг 1: 300 байт
python3 -c "print('G' * 299)" > "$BASEDIR/oscillate.txt"
sleep 3
check "[ -f '$BASEDIR/oscillate.txt.gz' ]" "EDGE-12a: 300 байт → .gz создана"

# Шаг 2: 150 байт
python3 -c "print('H' * 149)" > "$BASEDIR/oscillate.txt"
sleep 3
check "[ ! -f '$BASEDIR/oscillate.txt.gz' ]" "EDGE-12b: 150 байт → .gz удалена"

# Шаг 3: 400 байт
python3 -c "print('I' * 399)" > "$BASEDIR/oscillate.txt"
sleep 3
check "[ -f '$BASEDIR/oscillate.txt.gz' ]" "EDGE-12c: 400 байт → .gz создана снова"

# Шаг 4: 100 байт
python3 -c "print('J' * 99)" > "$BASEDIR/oscillate.txt"
sleep 3
check "[ ! -f '$BASEDIR/oscillate.txt.gz' ]" "EDGE-12d: 100 байт → .gz удалена снова"

# ===========================================================================
log "=== EDGE-13: algorithms=all, проверка однократного чтения ==="
# Проверяем что файл прочитан 1 раз, не 2
# Через логи: должно быть одно "Compressing:" на файл, не два
sudo systemctl stop "$SERVICE"
python3 -c "print('K' * 500)" > "$BASEDIR/single_read_test.txt"
sudo systemctl start "$SERVICE"
sleep 5

COMPRESS_LOGS=$(sudo journalctl -u "$SERVICE" --no-pager -n 500 | grep -c "single_read_test" || true)
check "[ $COMPRESS_LOGS -le 2 ]" "EDGE-13: Однократное чтение (логов: $COMPRESS_LOGS, ожидалось ≤2)"

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ EDGE: $PASS/$TOTAL пройдено, $FAIL провалено ==="

exit $FAIL
