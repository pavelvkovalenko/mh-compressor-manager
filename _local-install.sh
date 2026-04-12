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

# 5. Перезагрузка демонов systemd
sudo systemctl daemon-reload


sudo systemctl enable mh-compressor-manager.service
sudo systemctl start mh-compressor-manager
