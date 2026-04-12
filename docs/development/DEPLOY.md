# Сборка и деплой — mh-compressor-manager

## Сервер разработки
- **Подключение:** `ssh www` (SSH-ключи уже настроены)
- **Путь к проекту:** `/home/kane/Projects/mh-compressor-manager`

## Локальная сборка
```bash
./_build.sh
```

## Удалённая сборка
Перед сборкой синхронизировать изменения:
```bash
rsync -avz --exclude='.git' ./ www:/home/kane/Projects/mh-compressor-manager/
```

Сборка на сервере:
```bash
ssh www "cd /home/kane/Projects/mh-compressor-manager && bash _build.sh"
```

## Установка
```bash
ssh www "cd /home/kane/Projects/mh-compressor-manager && sudo bash _install.sh"
```

## Управление сервисом
```bash
# Перезапуск
ssh www "sudo systemctl restart mh-compressor-manager"

# Статус
ssh www "sudo systemctl status mh-compressor-manager --no-pager"

# Логи
ssh www "journalctl -u mh-compressor-manager --no-pager -n 50"

# Ошибки в логах
ssh www "journalctl -u mh-compressor-manager --no-pager | grep -iE 'error|fail|panic'"
```

## Порядок развёртывания
1. Запушить изменения на GitHub
2. Синхронизировать с сервером: `rsync`
3. Собрать: `bash _build.sh`
4. Установить: `sudo bash _install.sh`
5. Перезапустить: `sudo systemctl restart mh-compressor-manager`
6. Проверить статус и логи

## Зависимости для сборки (Fedora 43+)

```bash
sudo dnf install \
  cmake gcc-c++ \
  zlib-ng-compat-devel brotli-devel systemd-devel libselinux-devel \
  libdeflate-devel fmt-devel liburing-devel numactl-devel libseccomp-devel libcap-devel \
  pkgconf-pkg-config rpm-build
```

> **Примечание:** `_build.sh` автоматически проверяет все зависимости. Для Fedora 38-42 замените `zlib-ng-compat-devel` на `zlib-devel`.

## Известные проблемы сборки
- **GCC 15:** `fs::last_write_time()` несовместим с `__file_clock`. Использовать `utimensat()`.
- **safe.directory:** При ошибке `dubious ownership` выполнить:
  ```bash
  git config --global --add safe.directory /home/kane/Projects/mh-compressor-manager
  ```
