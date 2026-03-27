#!/bin/bash
sudo systemctl stop mh-compressor-manager >/dev/null
# 1. Копирование бинарного файла
sudo cp build/mh-compressor-manager /usr/bin/

# 2. Создание директории для конфига
sudo mkdir -p /etc/mediahive/

# 3. Копирование конфигурации (права 644 по ТЗ п.7.1)
sudo cp -iu compressor-manager.conf /etc/mediahive/
sudo chmod 644 /etc/mediahive/compressor-manager.conf

# 4. Установка systemd-юнита
sudo cp mh-compressor-manager.service /usr/lib/systemd/system/

# 5. Перезагрузка демонов systemd
sudo systemctl daemon-reload


sudo systemctl enable mh-compressor-manager.service
sudo systemctl start mh-compressor-manager