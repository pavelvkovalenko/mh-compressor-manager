#!/bin/bash
set -euo pipefail
sudo systemctl stop mh-compressor-manager >/dev/null

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

# 4. Установка systemd-юнита
sudo cp mh-compressor-manager.service /usr/lib/systemd/system/

# 5. Перезагрузка демонов systemd
sudo systemctl daemon-reload


sudo systemctl enable mh-compressor-manager.service
sudo systemctl start mh-compressor-manager