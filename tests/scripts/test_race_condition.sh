#!/bin/bash
# =============================================================================
# test_race_condition.sh — Тестирование race condition (ТЗ §18.6.6)
# Проверяет RACE-1 через RACE-17
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/race_test"
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
sudo systemctl start "$SERVICE"
sleep 2

# ===========================================================================
log "=== RACE-1: Маленький файл (5 КБ) удалён во время сжатия ==="
python3 -c "print('A' * 5000)" > "$BASEDIR/race_delete.txt"
# Удаляем почти сразу
sleep 0.1
rm -f "$BASEDIR/race_delete.txt"
sleep 5

# Не должно быть полузаписанных или orphan файлов
TMP_FILES=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TMP_FILES -eq 0 ]" "RACE-1a: нет временных файлов ($TMP_FILES)"

# ===========================================================================
log "=== RACE-2: Маленький файл изменён во время сжатия ==="
python3 -c "print('B' * 5000)" > "$BASEDIR/race_modify.txt"
sleep 0.2
python3 -c "print('C' * 5000)" > "$BASEDIR/race_modify.txt"
sleep 5

# Файл должен быть сжат (последняя версия)
check "[ -f '$BASEDIR/race_modify.txt.gz' ]" "RACE-2a: .gz создана после модификации"

# ===========================================================================
log "=== RACE-3: Маленький файл переименован во время сжатия ==="
python3 -c "print('D' * 5000)" > "$BASEDIR/race_rename_old.txt"
sleep 0.2
mv "$BASEDIR/race_rename_old.txt" "$BASEDIR/race_rename_new.txt"
sleep 5

# Сжатый файл должен быть на НОВОМ пути
check "[ -f '$BASEDIR/race_rename_new.txt.gz' ]" "RACE-3a: .gz на новом пути создана"
check "[ ! -f '$BASEDIR/race_rename_old.txt.gz' ]" "RACE-3b: .gz на старом пути нет"

# ===========================================================================
log "=== RACE-4: Маленький файл перемещён наружу во время сжатия ==="
python3 -c "print('E' * 5000)" > "$BASEDIR/race_move_out.txt"
sleep 0.2
mv "$BASEDIR/race_move_out.txt" /tmp/race_moved_out.txt
sleep 5

check "[ ! -f '$BASEDIR/race_move_out.txt.gz' ]" "RACE-4a: .gz в monitored не создана"
check "[ ! -f '/tmp/race_moved_out.txt.gz' ]" "RACE-4b: .gz вне monitored не создана"

# ===========================================================================
log "=== RACE-5: Маленький файл перемещён внутрь извне во время сжатия ==="
python3 -c "print('E' * 5000)" > /tmp/race_move_in.txt
sleep 0.2
mv /tmp/race_move_in.txt "$BASEDIR/race_moved_in.txt"
sleep 5

check "[ -f '$BASEDIR/race_moved_in.txt.gz' ]" "RACE-5a: .gz создана после перемещения внутрь"

# ===========================================================================
log "=== RACE-6: Маленький файл уменьшен до < порога во время сжатия ==="
python3 -c "print('F' * 5000)" > "$BASEDIR/race_shrink.txt"
sleep 0.2
python3 -c "print('G' * 100)" > "$BASEDIR/race_shrink.txt"
sleep 5

check "[ ! -f '$BASEDIR/race_shrink.txt.gz' ]" "RACE-6a: .gz не создана для файла < порога"

# ===========================================================================
log "=== RACE-7: Файл заменён другим файлом (same path, different content) ==="
python3 -c "print('H' * 5000)" > "$BASEDIR/race_replace.txt"
sleep 0.2
python3 -c "print('I' * 5000)" > "$BASEDIR/race_replace.txt"
sleep 5

# Должна быть сжата последняя версия
check "[ -f '$BASEDIR/race_replace.txt.gz' ]" "RACE-7a: .gz создана для финальной версии"

# ===========================================================================
log "=== RACE-8: Большой файл (10 МБ) удалён во время сжатия ==="
python3 -c "print('J' * 10000000)" > "$BASEDIR/race_big_delete.txt"
sleep 0.5
rm -f "$BASEDIR/race_big_delete.txt"
sleep 10

TMP_FILES=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TMP_FILES -eq 0 ]" "RACE-8a: нет временных файлов после удаления большого ($TMP_FILES)"

# ===========================================================================
log "=== RACE-9: Большой файл (10 МБ) изменён во время сжатия ==="
python3 -c "print('K' * 10000000)" > "$BASEDIR/race_big_modify.txt"
sleep 0.5
python3 -c "print('L' * 10000000)" >> "$BASEDIR/race_big_modify.txt"
sleep 15

# Файл должен быть сжат (возможно, дважды — текущее + повторное)
check "[ -f '$BASEDIR/race_big_modify.txt.gz' ]" "RACE-9a: .gz создана после модификации большого файла"

# ===========================================================================
log "=== RACE-10: Большой файл (10 МБ) переименован во время сжатия ==="
python3 -c "print('N' * 10000000)" > "$BASEDIR/race_big_rename.txt"
sleep 1
mv "$BASEDIR/race_big_rename.txt" "$BASEDIR/race_big_renamed.txt"
sleep 15

check "[ ! -f '$BASEDIR/race_big_rename.txt.gz' ]" "RACE-10a: старый путь чист"
# Файл на новом пути должен быть сжат
check "[ -f '$BASEDIR/race_big_renamed.txt.gz' ]" "RACE-10b: .gz создана на новом пути"

# ===========================================================================
log "=== RACE-11: Большой файл (10 МБ) перемещён в другую monitored-директорию ==="
mkdir -p "$BASEDIR/other_monitored"
python3 -c "print('O' * 10000000)" > "$BASEDIR/race_big_move.txt"
sleep 1
mv "$BASEDIR/race_big_move.txt" "$BASEDIR/other_monitored/race_big_moved.txt"
sleep 15

check "[ -f '$BASEDIR/other_monitored/race_big_moved.txt.gz' ]" "RACE-11a: .gz создана на новом месте"

# ===========================================================================
log "=== RACE-12: Большой файл (10 МБ) перемещён наружу из monitored-директории ==="
python3 -c "print('P' * 10000000)" > "$BASEDIR/race_big_out.txt"
sleep 1
mv "$BASEDIR/race_big_out.txt" /tmp/race_big_external.txt
sleep 15

check "[ ! -f '$BASEDIR/race_big_out.txt.gz' ]" "RACE-12a: .gz в monitored не создана"
rm -f /tmp/race_big_external.txt /tmp/race_big_external.txt.gz /tmp/race_big_external.txt.br

# ===========================================================================
log "=== RACE-13: Большой файл (10 МБ) изменён 3 раза подряд во время сжатия ==="
python3 -c "print('Q' * 10000000)" > "$BASEDIR/race_big_triple.txt"
sleep 0.5
python3 -c "print('R' * 10000000)" > "$BASEDIR/race_big_triple.txt"
sleep 0.5
python3 -c "print('S' * 10000000)" > "$BASEDIR/race_big_triple.txt"
sleep 0.5
python3 -c "print('T' * 10000000)" > "$BASEDIR/race_big_triple.txt"
sleep 15

check "[ -f '$BASEDIR/race_big_triple.txt.gz' ]" "RACE-13a: .gz создана после серии модификаций"

# ===========================================================================
log "=== RACE-14: 10 файлов одновременно удалены во время сжатия ==="
for i in $(seq 1 10); do
    python3 -c "print('M' * 5000)" > "$BASEDIR/race_batch_${i}.txt"
done
sleep 0.2
for i in $(seq 1 10); do
    rm -f "$BASEDIR/race_batch_${i}.txt"
done
sleep 5

TMP_FILES=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TMP_FILES -eq 0 ]" "RACE-14a: нет временных файлов после batch delete ($TMP_FILES)"

ORPHAN_GZ=$(sudo find "$BASEDIR" -name "race_batch_*.txt.gz" | wc -l)
check "[ $ORPHAN_GZ -eq 0 ]" "RACE-14b: нет orphan .gz файлов ($ORPHAN_GZ)"

# ===========================================================================
log "=== RACE-15: 10 файлов одновременно изменены во время сжатия ==="
for i in $(seq 1 10); do
    python3 -c "print('N' * 5000)" > "$BASEDIR/race_batch_mod_${i}.txt"
done
sleep 0.2
for i in $(seq 1 10); do
    python3 -c "print('O' * 5000)" > "$BASEDIR/race_batch_mod_${i}.txt"
done
sleep 5

PROCESSED=$(sudo find "$BASEDIR" -name "race_batch_mod_*.txt.gz" | wc -l)
check "[ $PROCESSED -ge 8 ]" "RACE-15a: $PROCESSED/10 файлов обработаны после batch modify"

# ===========================================================================
log "=== RACE-16: Файл изменён, затем удалён через 5мс ==="
python3 -c "print('P' * 5000)" > "$BASEDIR/race_change_delete.txt"
sleep 0.1
python3 -c "print('Q' * 5000)" > "$BASEDIR/race_change_delete.txt"
sleep 0.005
rm -f "$BASEDIR/race_change_delete.txt"
sleep 5

TMP_FILES=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TMP_FILES -eq 0 ]" "RACE-16a: нет временных файлов ($TMP_FILES)"
check "[ ! -f '$BASEDIR/race_change_delete.txt.gz' ]" "RACE-16b: .gz не создана"

# ===========================================================================
log "=== RACE-17: Файл переименован, затем изменён через 5мс ==="
python3 -c "print('R' * 5000)" > "$BASEDIR/race_rename_change_old.txt"
sleep 0.1
mv "$BASEDIR/race_rename_change_old.txt" "$BASEDIR/race_rename_change_new.txt"
sleep 0.005
python3 -c "print('S' * 5000)" > "$BASEDIR/race_rename_change_new.txt"
sleep 5

check "[ -f '$BASEDIR/race_rename_change_new.txt.gz' ]" "RACE-17a: .gz на новом пути создана"
check "[ ! -f '$BASEDIR/race_rename_change_old.txt.gz' ]" "RACE-17b: .gz на старом пути нет"

# ===========================================================================
log "=== Проверка отсутствия orphan-файлов ==="
TOTAL_ORPHANS=$(sudo find "$BASEDIR" \( -name "*.gz" -o -name "*.br" \) -exec sh -c '
    ORIG="${1%.gz}"
    ORIG="${ORIG%.br}"
    [ ! -f "$ORIG" ] && echo "$1"
' _ {} \; | wc -l)
check "[ $TOTAL_ORPHANS -eq 0 ]" "ORPHAN: $TOTAL_ORPHANS orphan сжатых файлов без оригинала"

# Проверка временных файлов
TOTAL_TMP=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TOTAL_TMP -eq 0 ]" "TEMP: $TOTAL_TMP временных файлов"

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ RACE CONDITION: $PASS/$TOTAL пройдено, $FAIL провалено ==="

exit $FAIL
