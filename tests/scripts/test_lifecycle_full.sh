#!/bin/bash
# =============================================================================
# test_lifecycle_full.sh — Полная проверка lifecycle (ТЗ §18.6.5)
# 10-шаговый сценарий: создание → переименование → перемещение → external changes → проверка
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/lifecycle_test"
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
    sudo rm -rf "$BASEDIR" /tmp/lifecycle_external /tmp/lifecycle_moved
}
trap cleanup EXIT

# ===========================================================================
log "=== Подготовка ==="
cleanup
sudo mkdir -p "$BASEDIR" "$BASEDIR/subdir"

# ===========================================================================
# ШАГ 1: Создать 50 файлов, запустить → все сжаты
# ===========================================================================
log "=== ШАГ 1: Создание 50 файлов и запуск ==="
for i in $(seq 1 50); do
    python3 -c "print('A' * 500)" > "$BASEDIR/file_${i}.txt"
done

sudo systemctl start "$SERVICE"
sleep 15

STEP1_GZ=$(sudo find "$BASEDIR" -name "file_*.txt.gz" | wc -l)
STEP1_BR=$(sudo find "$BASEDIR" -name "file_*.txt.br" | wc -l)
check "[ $STEP1_GZ -ge 45 ]" "ШАГ 1a: $STEP1_GZ/50 .gz созданы"
check "[ $STEP1_BR -ge 45 ]" "ШАГ 1b: $STEP1_BR/50 .br созданы"

# ===========================================================================
# ШАГ 2: Переименовать 10 файлов внутри → stale удалены, новые сжаты
# ===========================================================================
log "=== ШАГ 2: Переименование 10 файлов внутри ==="
for i in $(seq 1 10); do
    mv "$BASEDIR/file_${i}.txt" "$BASEDIR/renamed_${i}.txt"
done
sleep 10

STEP2_OLD_GZ=$(sudo find "$BASEDIR" -name "file_[1-9].txt.gz" -o -name "file_10.txt.gz" | wc -l)
STEP2_NEW_GZ=$(sudo find "$BASEDIR" -name "renamed_[1-9].txt.gz" -o -name "renamed_10.txt.gz" | wc -l)
check "[ $STEP2_OLD_GZ -eq 0 ]" "ШАГ 2a: 0 stale .gz на старых путях"
check "[ $STEP2_NEW_GZ -ge 8 ]" "ШАГ 2b: $STEP2_NEW_GZ/10 .gz на новых путях"

# ===========================================================================
# ШАГ 3: Переместить 10 файлов наружу → stale удалены
# ===========================================================================
log "=== ШАГ 3: Перемещение 10 файлов наружу ==="
sudo mkdir -p /tmp/lifecycle_moved
for i in $(seq 11 20); do
    mv "$BASEDIR/file_${i}.txt" /tmp/lifecycle_moved/
done
sleep 10

STEP3_STALE=$(sudo find "$BASEDIR" -name "file_[12][0-9].txt.gz" | wc -l)
check "[ $STEP3_STALE -eq 0 ]" "ШАГ 3a: 0 stale .gz после перемещения наружу"

# ===========================================================================
# ШАГ 4: Переместить 10 файлов внутрь → новые сжаты
# ===========================================================================
log "=== ШАГ 4: Перемещение 10 файлов извне внутрь ==="
sudo mkdir -p /tmp/lifecycle_external
for i in $(seq 1 10); do
    python3 -c "print('B' * 500)" > "/tmp/lifecycle_external/external_${i}.txt"
done

for i in $(seq 1 10); do
    mv "/tmp/lifecycle_external/external_${i}.txt" "$BASEDIR/"
done
sleep 10

STEP4_GZ=$(sudo find "$BASEDIR" -name "external_*.txt.gz" | wc -l)
check "[ $STEP4_GZ -ge 8 ]" "ШАГ 4a: $STEP4_GZ/10 перемещённых файлов сжаты"

# ===========================================================================
# ШАГ 5: Переместить папку наружу → stale внутри удалены
# ===========================================================================
log "=== ШАГ 5: Перемещение папки наружу ==="
for i in $(seq 1 10); do
    python3 -c "print('C' * 500)" > "$BASEDIR/subdir/sub_${i}.txt"
done
sleep 5

mv "$BASEDIR/subdir" /tmp/lifecycle_subdir_out
sleep 10

STEP5_STALE=$(sudo find "$BASEDIR/subdir" -name "*.gz" 2>/dev/null | wc -l)
check "[ $STEP5_STALE -eq 0 ]" "ШАГ 5a: 0 stale .gz в удалённой папке"

# ===========================================================================
# ШАГ 6: Переместить папку внутрь → все файлы обработаны
# ===========================================================================
log "=== ШАГ 6: Перемещение папки внутрь ==="
sudo mkdir -p /tmp/lifecycle_incoming
for i in $(seq 1 10); do
    python3 -c "print('D' * 500)" > "/tmp/lifecycle_incoming/incoming_${i}.txt"
done

mv /tmp/lifecycle_incoming "$BASEDIR/incoming"
sleep 10

STEP6_GZ=$(sudo find "$BASEDIR/incoming" -name "*.gz" | wc -l)
check "[ $STEP6_GZ -ge 8 ]" "ШАГ 6a: $STEP6_GZ/10 файлов в новой папке сжаты"

# ===========================================================================
# ШАГ 7: Остановить программу
# ===========================================================================
log "=== ШАГ 7: Остановка программы ==="
sudo systemctl stop "$SERVICE"
STATUS=$(sudo systemctl is-active "$SERVICE" 2>/dev/null || echo "inactive")
check "[ '$STATUS' = 'inactive' ]" "ШАГ 7a: сервис остановлен"

# ===========================================================================
# ШАГ 8: Externally — переименовать, переместить, удалить
# ===========================================================================
log "=== ШАГ 8: External changes ==="
# Переименовать 3 файла
for i in $(seq 21 23); do
    [ -f "$BASEDIR/file_${i}.txt" ] && mv "$BASEDIR/file_${i}.txt" "$BASEDIR/external_rename_${i}.txt"
done

# Удалить 3 файла
for i in $(seq 24 26); do
    rm -f "$BASEDIR/file_${i}.txt" "$BASEDIR/file_${i}.txt.gz" "$BASEDIR/file_${i}.txt.br"
done

# Переместить папку
[ -d "/tmp/lifecycle_subdir_out" ] && mv /tmp/lifecycle_subdir_out "$BASEDIR/subdir_back"

log "  External changes applied"

# ===========================================================================
# ШАГ 9: Запустить → stale удалены, новые сжаты
# ===========================================================================
log "=== ШАГ 9: Запуск после external changes ==="
sudo systemctl start "$SERVICE"
sleep 15

# Проверка переименованных
STEP9_RENAMED_GZ=$(sudo find "$BASEDIR" -name "external_rename_*.txt.gz" | wc -l)
check "[ $STEP9_RENAMED_GZ -ge 2 ]" "ШАГ 9a: $STEP9_RENAMED_GZ/3 переименованных файлов сжаты"

# Проверка что stale на старых путях удалены
STEP9_STALE_RENAMED=$(sudo find "$BASEDIR" -name "file_[2][123].txt.gz" | wc -l)
check "[ $STEP9_STALE_RENAMED -eq 0 ]" "ШАГ 9b: 0 stale .gz на старых путях переименованных"

# Проверка удалённых — не должно быть orphan
STEP9_ORPHAN_DELETED=$(sudo find "$BASEDIR" -name "file_[2][456].txt.gz" | wc -l)
check "[ $STEP9_ORPHAN_DELETED -eq 0 ]" "ШАГ 9c: 0 orphan .gz удалённых файлов"

# ===========================================================================
# ШАГ 10: Проверить отсутствие orphan-файлов
# ===========================================================================
log "=== ШАГ 10: Проверка orphan-файлов ==="

TOTAL_GZ=$(sudo find "$BASEDIR" -name "*.gz" | wc -l)
TOTAL_ORIG=$(sudo find "$BASEDIR" -type f ! -name "*.gz" ! -name "*.br" | wc -l)
log "  Итого: $TOTAL_ORIG оригиналов, $TOTAL_GZ .gz файлов"

# Находим orphan .gz/.br без оригинала
ORPHANS=$(sudo find "$BASEDIR" \( -name "*.gz" -o -name "*.br" \) -exec sh -c '
    ORIG="${1%.gz}"
    ORIG="${ORIG%.br}"
    [ ! -f "$ORIG" ] && echo "$1"
' _ {} \;)

ORPHAN_COUNT=$(echo "$ORPHANS" | grep -c "." || true)
check "[ $ORPHAN_COUNT -eq 0 ]" "ШАГ 10a: $ORPHAN_COUNT orphan сжатых файлов"

# Находим временные файлы
TMP_COUNT=$(sudo find "$BASEDIR" -name "*.tmp" | wc -l)
check "[ $TMP_COUNT -eq 0 ]" "ШАГ 10b: $TMP_COUNT временных файлов"

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ LIFECYCLE: $PASS/$TOTAL пройдено, $FAIL провалено ==="

exit $FAIL
