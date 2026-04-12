#!/bin/bash
# =============================================================================
# test_stale_cleanup.sh — Тестирование очистки stale-файлов (ТЗ §18.6.3)
# Проверяет STALE-1 через STALE-6
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/stale_test"
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
log "=== STALE-1: Initial scan — файл 1025 байт → сжат → уменьшен до 171 байт ==="
python3 -c "print('A' * 1024)" > "$BASEDIR/test1.txt"
sudo systemctl start "$SERVICE"
sleep 5  # Ожидание сжатия

check "[ -f '$BASEDIR/test1.txt.gz' ]" "STALE-1a: .gz созданан"
check "[ -f '$BASEDIR/test1.txt.br' ]" "STALE-1b: .br создана"

# Уменьшаем файл externally
python3 -c "print('B' * 170)" > "$BASEDIR/test1.txt"
sleep 3

# Перезапускаем — initial scan должен удалить stale
sudo systemctl restart "$SERVICE"
sleep 5

check "[ ! -f '$BASEDIR/test1.txt.gz' ]" "STALE-1c: stale .gz удалена после restart"
check "[ ! -f '$BASEDIR/test1.txt.br' ]" "STALE-1d: stale .br удалена после restart"

# ===========================================================================
log "=== STALE-2: Runtime — файл 500 байт сжат → модифицирован до 200 байт ==="
python3 -c "print('C' * 499)" > "$BASEDIR/test2.txt"
sleep 5

check "[ -f '$BASEDIR/test2.txt.gz' ]" "STALE-2a: .gz создана"

# Модифицируем до размера ниже порога
python3 -c "print('D' * 199)" > "$BASEDIR/test2.txt"
sleep 5

check "[ ! -f '$BASEDIR/test2.txt.gz' ]" "STALE-2b: stale .gz удалена при runtime модификации"
check "[ ! -f '$BASEDIR/test2.txt.br' ]" "STALE-2c: stale .br не создана"

# ===========================================================================
log "=== STALE-3: Файл 300 байт сжат → заменён файлом 100 байт с тем же mtime ==="
python3 -c "print('E' * 299)" > "$BASEDIR/test3.txt"
sleep 5

check "[ -f '$BASEDIR/test3.txt.gz' ]" "STALE-3a: .gz создана"

# Заменяем с тем же mtime
OLD_MTIME=$(stat -c %Y "$BASEDIR/test3.txt.gz")
python3 -c "print('F' * 99)" > "$BASEDIR/test3.txt"
touch -r "$BASEDIR/test3.txt.gz" "$BASEDIR/test3.txt" 2>/dev/null || true
sleep 5

# Stale должна быть удалена несмотря на совпадение mtime (проверка по размеру)
check "[ ! -f '$BASEDIR/test3.txt.gz' ]" "STALE-3b: stale .gz удалена (проверка по размеру, не только mtime)"

# ===========================================================================
log "=== STALE-4: 50 файлов стали ниже порога одновременно ==="
for i in $(seq 1 50); do
    python3 -c "print('G' * 499)" > "$BASEDIR/bulk_${i}.txt"
done
sleep 10  # Ожидание сжатия всех

# Уменьшаем все 50
for i in $(seq 1 50); do
    python3 -c "print('H' * 100)" > "$BASEDIR/bulk_${i}.txt"
done
sleep 5

STALE_COUNT=$(sudo find "$BASEDIR" -name "bulk_*.txt.gz" -o -name "bulk_*.txt.br" | wc -l)
check "[ $STALE_COUNT -eq 0 ]" "STALE-4: Все 50 stale-копий удалены (осталось $STALE_COUNT)"

# ===========================================================================
log "=== STALE-5: Файл 200 байт (ниже порога), stale существует ==="
python3 -c "print('I' * 199)" > "$BASEDIR/existing_stale.txt"
# Создаём stale вручную
echo "stale data" | gzip > "$BASEDIR/existing_stale.txt.gz"
touch -t 202001010000 "$BASEDIR/existing_stale.txt.gz"

sleep 5

check "[ ! -f '$BASEDIR/existing_stale.txt.gz' ]" "STALE-5: stale .gz удалена для файла ниже порога"

# ===========================================================================
log "=== STALE-6: Файл ниже порога, stale нет — без действий ==="
python3 -c "print('J' * 150)" > "$BASEDIR/no_stale.txt"
sleep 3

# Никаких действий — файл просто игнорируется
check "[ ! -f '$BASEDIR/no_stale.txt.gz' ]" "STALE-6a: .gz не создана"
check "[ ! -f '$BASEDIR/no_stale.txt.br' ]" "STALE-6b: .br не создана"

# Файл должен остаться нетронутым
check "[ -f '$BASEDIR/no_stale.txt' ]" "STALE-6c: оригинал не удалён"

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ STALE: $PASS/$TOTAL пройдено, $FAIL провалено ==="

# Проверка логов
log "=== Проверка логов ==="
LOG_ENTRIES=$(sudo journalctl -u "$SERVICE" --no-pager -n 200 2>/dev/null | grep -i "stale\|below.*threshold\|порог" | wc -l)
log "  Записей о stale в логах: $LOG_ENTRIES"
check "[ $LOG_ENTRIES -gt 0 ]" "Логи содержат записи о stale-файлах"

exit $FAIL
