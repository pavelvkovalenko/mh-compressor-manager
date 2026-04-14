#!/bin/bash
# =============================================================================
# test_move_rename.sh — Тестирование перемещений и переименований (ТЗ §18.6.4)
# Проверяет MOVE-1 через MOVE-15
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/move_test"
SERVICE="mh-compressor-manager"
PASS=0
FAIL=0
TOTAL=0

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }
check() {
    TOTAL=$((TOTAL + 1))
    if bash -c "$1" 2>/dev/null; then
        PASS=$((PASS + 1))
        echo "  ✅ PASS [$TOTAL]: $2"
    else
        FAIL=$((FAIL + 1))
        echo "  ❌ FAIL [$TOTAL]: $2"
    fi
}

cleanup() {
    sudo systemctl stop "$SERVICE" 2>/dev/null || true
    sudo rm -rf "$BASEDIR" /tmp/move_external /tmp/move_incoming
}
trap cleanup EXIT

# ===========================================================================
log "=== Подготовка ==="
cleanup
sudo mkdir -p "$BASEDIR" "$BASEDIR/dir1" "$BASEDIR/dir2"
sudo systemctl start "$SERVICE"
sleep 2

# ===========================================================================
log "=== MOVE-1: Переименование файла внутри monitored-директории ==="
python3 -c "print('A' * 500)" > "$BASEDIR/original.txt"
sleep 5

check "[ -f '$BASEDIR/original.txt.gz' ]" "MOVE-1a: original.txt.gz создана"

mv "$BASEDIR/original.txt" "$BASEDIR/renamed.txt"
sleep 5

check "[ ! -f '$BASEDIR/original.txt.gz' ]" "MOVE-1b: stale original.txt.gz удалена"
check "[ -f '$BASEDIR/renamed.txt.gz' ]" "MOVE-1c: renamed.txt.gz создана"

# ===========================================================================
log "=== MOVE-2: Переименование файла ниже порога ==="
python3 -c "print('B' * 500)" > "$BASEDIR/big.txt"
sleep 5

check "[ -f '$BASEDIR/big.txt.gz' ]" "MOVE-2a: .gz создана"

# Переименование + замена содержимого на файл ниже порога
mv "$BASEDIR/big.txt" "$BASEDIR/small.txt"
python3 -c "print('C' * 100)" > "$BASEDIR/small.txt"
sleep 5

check "[ ! -f '$BASEDIR/small.txt.gz' ]" "MOVE-2b: файл ниже порога не сжат"
check "[ ! -f '$BASEDIR/big.txt.gz' ]" "MOVE-2c: stale big.txt.gz удалена"

# ===========================================================================
log "=== MOVE-3: Перемещение файла из monitored-директории наружу ==="
python3 -c "print('D' * 500)" > "$BASEDIR/to_remove.txt"
sleep 5

check "[ -f '$BASEDIR/to_remove.txt.gz' ]" "MOVE-3a: .gz создана"

mv "$BASEDIR/to_remove.txt" /tmp/moved_out.txt
sleep 5

check "[ ! -f '$BASEDIR/to_remove.txt.gz' ]" "MOVE-3b: stale .gz удалена после перемещения наружу"
check "[ ! -f '/tmp/moved_out.txt.gz' ]" "MOVE-3c: .gz не создана вне monitored-директории"

# ===========================================================================
log "=== MOVE-4: Перемещение файла извне в monitored-директорию ==="
python3 -c "print('E' * 500)" > /tmp/moved_in.txt
sleep 2

mv /tmp/moved_in.txt "$BASEDIR/moved_in.txt"
sleep 5

check "[ -f '$BASEDIR/moved_in.txt.gz' ]" "MOVE-4a: перемещённый файл сжат (.gz)"
check "[ -f '$BASEDIR/moved_in.txt.br' ]" "MOVE-4b: перемещённый файл сжат (.br)"

# ===========================================================================
log "=== MOVE-5: Перемещение папки dir1/ → dir2/ внутри monitored ==="
for i in $(seq 1 10); do
    python3 -c "print('F' * 500)" > "$BASEDIR/dir1/file_${i}.txt"
done
sleep 5

DIR1_GZ=$(sudo find "$BASEDIR/dir1" -name "*.gz" | wc -l)
check "[ $DIR1_GZ -ge 5 ]" "MOVE-5a: $DIR1_GZ/10 .gz в dir1 до перемещения"

mv "$BASEDIR/dir1" "$BASEDIR/dir2/moved_dir"
sleep 5

DIR1_STALE=$(sudo find "$BASEDIR/dir1" -name "*.gz" 2>/dev/null | wc -l)
DIR2_GZ=$(sudo find "$BASEDIR/dir2/moved_dir" -name "*.gz" 2>/dev/null | wc -l)
check "[ $DIR1_STALE -eq 0 ]" "MOVE-5b: stale .gz в dir1 удалены (осталось $DIR1_STALE)"
check "[ $DIR2_GZ -ge 5 ]" "MOVE-5c: $DIR2_GZ/10 .gz в dir2/moved_dir после перемещения"

# ===========================================================================
log "=== MOVE-6: Перемещение папки monitored/subdir/ наружу ==="
sudo mkdir -p "$BASEDIR/subdir"
for i in $(seq 1 10); do
    python3 -c "print('G' * 500)" > "$BASEDIR/subdir/file_${i}.txt"
done
sleep 5

SUBDIR_GZ=$(sudo find "$BASEDIR/subdir" -name "*.gz" | wc -l)
check "[ $SUBDIR_GZ -ge 5 ]" "MOVE-6a: $SUBDIR_GZ/10 .gz в subdir до перемещения"

mv "$BASEDIR/subdir" /tmp/moved_subdir
sleep 5

SUBDIR_STALE=$(sudo find "$BASEDIR/subdir" -name "*.gz" 2>/dev/null | wc -l)
check "[ $SUBDIR_STALE -eq 0 ]" "MOVE-6b: stale .gz в subdir удалены (осталось $SUBDIR_STALE)"

# ===========================================================================
log "=== MOVE-7: Перемещение папки external/ в monitored/ ==="
sudo mkdir -p /tmp/external_dir
for i in $(seq 1 10); do
    python3 -c "print('H' * 500)" > "/tmp/external_dir/file_${i}.txt"
done

mv /tmp/external_dir "$BASEDIR/external_dir"
sleep 5

EXT_GZ=$(sudo find "$BASEDIR/external_dir" -name "*.gz" | wc -l)
check "[ $EXT_GZ -ge 5 ]" "MOVE-7a: $EXT_GZ/10 .gz в перемещённой папке"

# ===========================================================================
log "=== MOVE-8: Перемещение при неработающей программе ==="
sudo systemctl stop "$SERVICE"

# Создаём файл, сжимаем, перемещаем
python3 -c "print('I' * 500)" > "$BASEDIR/before_stop.txt"
sudo systemctl start "$SERVICE"
sleep 5

check "[ -f '$BASEDIR/before_stop.txt.gz' ]" "MOVE-8a: .gz создана до остановки"

# Останавливаем, перемещаем, запускаем
sudo systemctl stop "$SERVICE"
mv "$BASEDIR/before_stop.txt" "$BASEDIR/after_restart.txt"

sudo systemctl start "$SERVICE"
sleep 5

check "[ ! -f '$BASEDIR/before_stop.txt.gz' ]" "MOVE-8b: stale .gz на старом пути удалена"
check "[ -f '$BASEDIR/after_restart.txt.gz' ]" "MOVE-8c: .gz на новом пути создана"

# ===========================================================================
log "=== MOVE-9: Потеря события MOVED_TO (пришло только MOVED_FROM) ==="
python3 -c "print('J' * 500)" > "$BASEDIR/lost_move.txt"
sleep 5

check "[ -f '$BASEDIR/lost_move.txt.gz' ]" "MOVE-9a: .gz создана"

# Имитируем потерю: удаляем без MOVED_TO
rm "$BASEDIR/lost_move.txt"
sleep 5

check "[ ! -f '$BASEDIR/lost_move.txt.gz' ]" "MOVE-9b: stale .gz удалена при потере MOVED_TO"

# ===========================================================================
log "=== MOVE-10: Потеря события MOVED_FROM (пришло только MOVED_TO) ==="
python3 -c "print('K' * 500)" > /tmp/lost_from.txt
mv /tmp/lost_from.txt "$BASEDIR/lost_to.txt"
sleep 5

# Должен обработаться как новый файл
check "[ -f '$BASEDIR/lost_to.txt.gz' ]" "MOVE-10: файл обработан как новый при потере MOVED_FROM"

# ===========================================================================
log "=== MOVE-11: Перемещение папки с файлами разного размера ==="
sudo mkdir -p "$BASEDIR/mixed_dir"
# Крупные файлы
for i in $(seq 1 5); do
    python3 -c "print('L' * 500)" > "$BASEDIR/mixed_dir/big_${i}.txt"
done
# Мелкие файлы
for i in $(seq 1 5); do
    python3 -c "print('M' * 100)" > "$BASEDIR/mixed_dir/small_${i}.txt"
done
sleep 5

mv "$BASEDIR/mixed_dir" "$BASEDIR/mixed_dir_moved"
sleep 5

MIXED_BIG_GZ=$(sudo find "$BASEDIR/mixed_dir_moved" -name "big_*.txt.gz" | wc -l)
MIXED_SMALL_GZ=$(sudo find "$BASEDIR/mixed_dir_moved" -name "small_*.txt.gz" | wc -l)
check "[ $MIXED_BIG_GZ -ge 3 ]" "MOVE-11a: $MIXED_BIG_GZ/5 крупных файлов сжаты"
check "[ $MIXED_SMALL_GZ -eq 0 ]" "MOVE-11b: 0/5 мелких файлов сжаты"

# ===========================================================================
log "=== MOVE-12: Циклическое перемещение: dir1 → dir2 → dir3 ==="
sudo mkdir -p "$BASEDIR/cycle_a" "$BASEDIR/cycle_b" "$BASEDIR/cycle_c"

python3 -c "print('N' * 500)" > "$BASEDIR/cycle_a/file.txt"
sleep 5

check "[ -f '$BASEDIR/cycle_a/file.txt.gz' ]" "MOVE-12a: .gz в cycle_a создана"

mv "$BASEDIR/cycle_a/file.txt" "$BASEDIR/cycle_b/file.txt"
sleep 5

check "[ ! -f '$BASEDIR/cycle_a/file.txt.gz' ]" "MOVE-12b: stale в cycle_a удалена"
check "[ -f '$BASEDIR/cycle_b/file.txt.gz' ]" "MOVE-12c: .gz в cycle_b создана"

mv "$BASEDIR/cycle_b/file.txt" "$BASEDIR/cycle_c/file.txt"
sleep 5

check "[ ! -f '$BASEDIR/cycle_b/file.txt.gz' ]" "MOVE-12d: stale в cycle_b удалена"
check "[ -f '$BASEDIR/cycle_c/file.txt.gz' ]" "MOVE-12e: .gz в cycle_c создана"

# ===========================================================================
log "=== MOVE-13: Переименование с тем же размером, но другим содержимым ==="
python3 -c "print('O' * 500)" > "$BASEDIR/same_size_old.txt"
sleep 5

check "[ -f '$BASEDIR/same_size_old.txt.gz' ]" "MOVE-13a: .gz создана"

# Переименовываем с другим содержимым (тот же размер)
python3 -c "print('P' * 500)" > "$BASEDIR/same_size_new.txt"
rm "$BASEDIR/same_size_old.txt" "$BASEDIR/same_size_old.txt.gz" "$BASEDIR/same_size_old.txt.br" 2>/dev/null || true
sleep 5

check "[ -f '$BASEDIR/same_size_new.txt.gz' ]" "MOVE-13b: .gz на новом пути создана"

# ===========================================================================
log "=== MOVE-14: Перемещение 100 файлов одновременно (batch move) ==="
sudo mkdir -p /tmp/batch_dir
for i in $(seq 1 100); do
    python3 -c "print('Q' * 500)" > "/tmp/batch_dir/file_${i}.txt"
done

mv /tmp/batch_dir "$BASEDIR/batch_dir"
sleep 15

BATCH_GZ=$(sudo find "$BASEDIR/batch_dir" -name "*.gz" | wc -l)
check "[ $BATCH_GZ -ge 90 ]" "MOVE-14: $BATCH_GZ/100 файлов обработаны после batch move"

# ===========================================================================
log "=== MOVE-15: Перемещение monitored-директории в другую monitored ==="
sudo mkdir -p "$BASEDIR/outer" "$BASEDIR/inner"

python3 -c "print('R' * 500)" > "$BASEDIR/outer/file.txt"
sleep 5

check "[ -f '$BASEDIR/outer/file.txt.gz' ]" "MOVE-15a: .gz в outer создана"

mv "$BASEDIR/outer" "$BASEDIR/inner/outer_nested"
sleep 5

NESTED_GZ=$(sudo find "$BASEDIR/inner/outer_nested" -name "*.gz" | wc -l)
check "[ $NESTED_GZ -ge 1 ]" "MOVE-15b: $NESTED_GZ .gz во вложенной директории"

# Проверка orphan-файлов
TOTAL_GZ=$(sudo find "$BASEDIR" -name "*.gz" | wc -l)
TOTAL_ORIG=$(sudo find "$BASEDIR" -type f ! -name "*.gz" ! -name "*.br" | wc -l)
log "  Итого: $TOTAL_ORIG оригиналов, $TOTAL_GZ .gz файлов"

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ MOVE/RENAME: $PASS/$TOTAL пройдено, $FAIL провалено ==="

# Проверка на orphan-файлы
ORPHANS=$(sudo find "$BASEDIR" -name "*.gz" -exec sh -c '
    ORIG="${1%.gz}"
    [ ! -f "$ORIG" ] && echo "$1"
' _ {} \; | wc -l)
check "[ $ORPHANS -eq 0 ]" "ORPHAN: $ORPHANS orphan .gz файлов без оригинала"

exit $FAIL
