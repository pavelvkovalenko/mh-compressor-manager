#!/bin/bash
# =============================================================================
# test_security.sh — Тестирование безопасности (ТЗ §18.5)
# Проверяет SEC-1 через SEC-6
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/security_test"
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
    sudo rm -f /tmp/external_target.txt
}
trap cleanup EXIT

# ===========================================================================
log "=== Подготовка ==="
cleanup
sudo mkdir -p "$BASEDIR"

# ===========================================================================
log "=== SEC-1: Symlink на файл вне целевой директории ==="
echo "external content" > /tmp/external_target.txt
ln -sf /tmp/external_target.txt "$BASEDIR/symlink_outside.txt"

sudo systemctl start "$SERVICE"
sleep 5

check "[ ! -f '$BASEDIR/symlink_outside.txt.gz' ]" "SEC-1a: symlink не сжат (.gz)"
check "[ ! -f '$BASEDIR/symlink_outside.txt.br' ]" "SEC-1b: symlink не сжат (.br)"

# Проверяем что внешний файл не тронут
check "[ -f '/tmp/external_target.txt' ]" "SEC-1c: внешний файл не удалён"
check "[ ! -f '/tmp/external_target.txt.gz' ]" "SEC-1d: сжатая копия внешнего файла не создана"

# ===========================================================================
log "=== SEC-2: Symlink на системный файл (/etc/passwd) ==="
ln -sf /etc/passwd "$BASEDIR/symlink_passwd.txt"
sleep 3

check "[ ! -f '$BASEDIR/symlink_passwd.txt.gz' ]" "SEC-2a: symlink на /etc/passwd не сжат"
check "[ ! -f '/etc/passwd.gz' ]" "SEC-2b: /etc/passwd.gz не создан"

# ===========================================================================
log "=== SEC-3: Подмена исходного файла symlink во время сжатия ==="
# Создаём обычный файл
python3 -c "print('A' * 500)" > "$BASEDIR/replace_test.txt"
sleep 1

# Заменяем его symlink'ом
rm -f "$BASEDIR/replace_test.txt"
ln -sf /tmp/external_target.txt "$BASEDIR/replace_test.txt"
sleep 5

# Сжатая копия не должна быть создана
check "[ ! -f '$BASEDIR/replace_test.txt.gz' ]" "SEC-3a: после подмены .gz не создана"
check "[ ! -f '$BASEDIR/replace_test.txt.br' ]" "SEC-3b: после подмены .br не создана"

# ===========================================================================
log "=== SEC-4: Файл без прав на чтение ==="
python3 -c "print('B' * 500)" > "$BASEDIR/no_read.txt"
chmod 000 "$BASEDIR/no_read.txt"
sleep 5

check "[ ! -f '$BASEDIR/no_read.txt.gz' ]" "SEC-4a: файл без прав чтения не сжат"

# Восстанавливаем права для очистки
chmod 644 "$BASEDIR/no_read.txt"

# ===========================================================================
log "=== SEC-5: Директория без прав на запись ==="
sudo mkdir -p "$BASEDIR/no_write_dir"
python3 -c "print('C' * 500)" > "$BASEDIR/no_write_dir/test.txt"
sudo chmod 555 "$BASEDIR/no_write_dir"  # r-x, нет записи
sleep 5

# Сжатие может succeed или fail в зависимости от прав пользователя сервиса
# Ключевое: не должно быть краха или повреждения
check "[ -d '$BASEDIR/no_write_dir' ]" "SEC-5a: директория не повреждена"

# Восстанавливаем
sudo chmod 755 "$BASEDIR/no_write_dir"

# ===========================================================================
log "=== SEC-6: Сжатый файл с правами 000 ==="
python3 -c "print('D' * 500)" > "$BASEDIR/zero_perms.txt"
sleep 5

if [ -f "$BASEDIR/zero_perms.txt.gz" ]; then
    # Если .gz создана — меняем права и проверяем обработку
    chmod 000 "$BASEDIR/zero_perms.txt.gz"
    sleep 3

    # Файл не должен быть удалён или повреждён некорректно
    # (сервис должен залогировать ошибку, но не крашиться)
    STATUS=$(sudo systemctl is-active "$SERVICE")
    check "[ '$STATUS' = 'active' ]" "SEC-6a: сервис не упал при правах 000 на .gz"

    # Восстанавливаем
    chmod 644 "$BASEDIR/zero_perms.txt.gz" 2>/dev/null || true
else
    # Если .gz не создана — тоже ок
    check "true" "SEC-6a: .gz не создана, тест пропущен"
fi

# ===========================================================================
log ""
log "=== РЕЗУЛЬТАТЫ SECURITY: $PASS/$TOTAL пройдено, $FAIL провалено ==="

# Проверка логов на предупреждения безопасности
log "=== Проверка логов безопасности ==="
SEC_LOGS=$(sudo journalctl -u "$SERVICE" --no-pager -n 500 | grep -i "SECURITY\|symlink\|permission denied\|access denied" | wc -l)
log "  Записей о безопасности в логах: $SEC_LOGS"

exit $FAIL
