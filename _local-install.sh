#!/bin/bash
set -euo pipefail

# Остановка сервиса (только если уже установлен — для обновлений)
sudo systemctl stop mh-compressor-manager 2>/dev/null || true

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT/src"

# 1. Копирование бинарного файла
sudo cp build/mh-compressor-manager /usr/bin/

# 2. Конфигурация: при обновлении — слияние с сохранением [general] и [folder_override:]
CONF_DIR=/etc/mediahive
CONF_NAME=compressor-manager.conf
sudo mkdir -p "$CONF_DIR"
if [[ -f "$CONF_DIR/$CONF_NAME" ]]; then
    MERGED=$(mktemp)
    bash ./merge-compressor-conf.sh ./compressor-manager.conf "$CONF_DIR/$CONF_NAME" "$MERGED"
    sudo cp -a "$CONF_DIR/$CONF_NAME" "$CONF_DIR/${CONF_NAME}.bak.$(date +%Y%m%d%H%M%S)"
    sudo cp "$MERGED" "$CONF_DIR/$CONF_NAME"
    rm -f "$MERGED"
else
    sudo cp compressor-manager.conf "$CONF_DIR/$CONF_NAME"
fi
sudo chmod 644 "$CONF_DIR/$CONF_NAME"

# 2б. Полная конфигурация с документацией (справочник, перезаписывается при обновлении)
if [[ -f "compressor-manager.conf.full" ]]; then
    sudo cp compressor-manager.conf.full "$CONF_DIR/${CONF_NAME}.full"
    sudo chmod 644 "$CONF_DIR/${CONF_NAME}.full"
fi

# 2в. Установка файлов переводов (.mo) — ТЗ §22.7
if [[ -f "translations/locale/ru/LC_MESSAGES/mh-compressor-manager.mo" ]]; then
    sudo mkdir -p /usr/share/locale/ru/LC_MESSAGES
    sudo cp translations/locale/ru/LC_MESSAGES/mh-compressor-manager.mo \
        /usr/share/locale/ru/LC_MESSAGES/
    sudo chmod 644 /usr/share/locale/ru/LC_MESSAGES/mh-compressor-manager.mo
fi

# 4. Установка systemd-юнита
sudo cp mh-compressor-manager.service /usr/lib/systemd/system/

# 5. Установка man-страниц (ТЗ §6.2)
if [[ -d "../man/en/man1" ]]; then
    sudo mkdir -p /usr/share/man/man1
    sudo cp ../man/en/man1/mh-compressor-manager.1 /usr/share/man/man1/
    sudo chmod 644 /usr/share/man/man1/mh-compressor-manager.1
fi
if [[ -d "../man/ru/man1" ]]; then
    sudo mkdir -p /usr/share/man/ru/man1
    sudo cp ../man/ru/man1/mh-compressor-manager.1 /usr/share/man/ru/man1/
    sudo chmod 644 /usr/share/man/ru/man1/mh-compressor-manager.1
fi
# Обновить кэш man-db
sudo mandb 2>/dev/null || true

# 6. Установка bash completion (ТЗ §6.3)
if [[ -f "../completion/mh-compressor-manager" ]]; then
    sudo mkdir -p /usr/share/bash-completion/completions
    sudo cp ../completion/mh-compressor-manager /usr/share/bash-completion/completions/
    sudo chmod 644 /usr/share/bash-completion/completions/mh-compressor-manager
fi

# 7. Перезагрузка демонов systemd
sudo systemctl daemon-reload


sudo systemctl enable mh-compressor-manager.service
sudo systemctl start mh-compressor-manager
