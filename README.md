# mh-compressor-manager

<div align="center">

![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-23-green.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20(Fedora%2038%2B)-orange.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)

**Автоматическое сжатие статического контента для nginx**

[Документация](#-документация) • [Установка](#-установка) • [Конфигурация](#конфигурация) • [API](#-интерфейс-командной-строки) • [Лицензия](#-лицензия)

</div>

---

## 📖 О проекте

**mh-compressor-manager** — это фоновая служба (демон) для автоматического сжатия статического контента веб-сервера nginx. Программа отслеживает изменения в файловой системе в реальном времени и создает сжатые копии файлов в форматах **Gzip (.gz)** и **Brotli (.br)**.

> ✅ **Цель:** Снижение сетевого трафика и ускорение загрузки веб-страниц за счет предварительного сжатия статических ресурсов.

### 📜 История разработки

| Версия | Год | Особенности |
|--------|-----|-------------|
| **0.x** | 2014 | Только Gzip, без логов, без многопоточности |
| **1.0.0** | 2026 | Gzip + Brotli, systemd, многопоточность, полное логирование |

> 📌 **Примечание:** Программа разработана специально для **Fedora 43** с учётом всех современных требований к безопасности и производительности.

---

## ✨ Возможности

- **🔍 Мониторинг в реальном времени** — использование `inotify` для отслеживания событий файловой системы
- **🗜️ Два алгоритма сжатия** — Gzip (zlib) и Brotli (libbrotli)
- **📁 Рекурсивное сканирование** — поддержка вложенных директорий
- **🧵 Многопоточность** — пул потоков для параллельного сжатия файлов
- **⏱️ Debounce-защита** — предотвращение множественной обработки при частых изменениях
- **♻️ Идемпотентность** — пропуск файлов, которые уже сжаты и актуальны
- **📋 Сохранение метаданных** — права доступа, владелец, группа, временные метки, SELinux-контекст
- **⚙️ Интеграция с systemd** — уведомления о готовности, корректное завершение работы
- **📝 Гибкая конфигурация** — INI-файл + аргументы командной строки
- **📊 Подробное логирование** — интеграция с syslog/journald

### Поддерживаемые расширения (по умолчанию)

| Расширение | Тип контента | Приоритет |
|------------|--------------|-----------|
| `txt` | Текстовые файлы | Высокий |
| `js` | JavaScript | Высокий |
| `css` | Таблицы стилей | Высокий |
| `svg` | Векторная графика | Высокий |
| `json` | Данные JSON | Высокий |
| `html`, `htm` | HTML-страницы | Высокий |
| `map` | Source maps | Средний |

---

## 📋 Требования

### Системные требования

| Компонент | Требование | Примечание |
|-----------|------------|------------|
| **ОС** | Linux (Fedora 43+, Fedora 38+, CentOS, Ubuntu) | Приоритет: Fedora 43 |
| **Компилятор** | GCC 13+ или Clang 16+ | Для поддержки C++23 |
| **CMake** | 3.20+ | Система сборки |
| **ОЗУ** | ≤ 50 МБ (покой), ≤ 200 МБ (пик) | Зависит от количества потоков |
| **inotify watchers** | ≥ 8192 (рекомендуется 524288) | Для мониторинга больших деревьев |

### Зависимости

> ⚠️ **Важно для Fedora 43:** В Fedora 43 пакет `zlib-devel` заменён на `zlib-ng-compat-devel`.

| Библиотека | Fedora 43+ | Fedora 38-42 | Ubuntu/Debian |
|------------|------------|--------------|---------------|
| **zlib** | `zlib-ng-compat-devel` | `zlib-devel` | `zlib1g-dev` |
| **Brotli** | `brotli-devel` | `brotli-devel` | `libbrotli-dev` |
| **systemd** | `systemd-devel` | `systemd-devel` | `libsystemd-dev` |
| **libselinux** | `libselinux-devel` | `libselinux-devel` | `libselinux1-dev` |
| **pkg-config** | `pkgconf-pkg-config` | `pkgconfig` | `pkg-config` |

---

## 🚀 Установка

### Быстрый старт (Fedora 43)

```bash
# 1. Установка зависимостей
sudo dnf install zlib-ng-compat-devel brotli-devel systemd-devel libselinux-devel pkgconf-pkg-config cmake gcc-c++ rpm-build

# 2. Сборка RPM-пакета
./build-rpm.sh 1.0.0

# 3. Установка пакета
sudo dnf install ~/rpmbuild/RPMS/x86_64/mh-compressor-manager-1.0.0-1.fc43.x86_64.rpm

# 4. Активация службы
sudo systemctl daemon-reload
sudo systemctl enable mh-compressor-manager
sudo systemctl start mh-compressor-manager
```

### Сборка из исходных кодов

```bash
# 1. Клонирование репозитория
git clone https://github.com/pavelvkovalenko/mh-compressor-manager.git
cd mh-compressor-manager

# 2. Создание директории сборки
mkdir build && cd build

# 3. Конфигурация и компиляция
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 4. Установка
sudo make install
sudo systemctl daemon-reload
```

### Структура установки

| Файл | Путь | Права |
|------|------|-------|
| Бинарный файл | `/usr/bin/mh-compressor-manager` | 755 |
| Конфигурация | `/etc/mediahive/compressor-manager.conf` | 644 |
| Systemd-юнит | `/usr/lib/systemd/system/mh-compressor-manager.service` | 644 |

---

## ⚙️ Конфигурация

### Файл конфигурации

Путь: `/etc/mediahive/compressor-manager.conf`

```ini
[general]
# Каталоги для мониторинга (разделитель ; или пробел)
target_path=/var/www/html;/var/www/site1

# Логирование отладки (true/false)
debug=false

# Количество потоков (0 = автоопределение по CPU)
threads=0

# Список расширений (без точек, через пробел)
list=txt js css svg json html htm map

# Алгоритмы сжатия: gzip, brotli, all
algorithms=all

# Уровень сжатия Gzip (1-9)
gzip_level=9

# Уровень сжатия Brotli (1-11)
brotli_level=11

# Задержка перед сжатием после изменения файла (сек)
debounce_delay=2
```

### Параметры конфигурации

| Параметр | Тип | По умолчанию | Описание |
|----------|-----|--------------|----------|
| `target_path` | string | `/var/www/html` | Каталоги для мониторинга |
| `debug` | boolean | `false` | Режим отладочного логирования |
| `threads` | integer | `0` (авто) | Количество потоков пула |
| `list` | string | `txt js css...` | Список расширений файлов |
| `algorithms` | string | `all` | Алгоритмы: `gzip`, `brotli`, `all` |
| `gzip_level` | integer | `9` | Уровень сжатия Gzip (1-9) |
| `brotli_level` | integer | `11` | Уровень сжатия Brotli (1-11) |
| `debounce_delay` | integer | `2` | Задержка перед сжатием (сек) |

> ⚠️ **Важно:** Аргументы командной строки имеют приоритет над настройками в файле конфигурации.

---

## 💻 Интерфейс командной строки

### Доступные аргументы

```bash
mh-compressor-manager [ОПЦИИ]
```

| Аргумент | Описание | Пример |
|----------|----------|--------|
| `--config <path>` | Путь к файлу конфигурации | `--config /etc/my.conf` |
| `--dir <path>` | Целевая директория | `--dir /var/www/html` |
| `--ext <list>` | Список расширений | `--ext "js css html"` |
| `--gzip-level <N>` | Уровень сжатия Gzip (1-9) | `--gzip-level 9` |
| `--brotli-level <N>` | Уровень сжатия Brotli (1-11) | `--brotli-level 11` |
| `--dry-run` | Режим проверки (без сжатия) | `--dry-run` |
| `--help`, `-h` | Вывод справки | `--help` |
| `--version`, `-v` | Вывод версии | `--version` |

### Примеры использования

```bash
# Запуск с настройками по умолчанию
sudo mh-compressor-manager

# Запуск в режиме отладки
sudo mh-compressor-manager --debug --dir /var/www/mysite

# Тестовый запуск без реального сжатия
sudo mh-compressor-manager --dry-run --debug

# Запуск с переопределением уровней сжатия
sudo mh-compressor-manager --gzip-level 9 --brotli-level 11

# Просмотр версии
mh-compressor-manager --version

# Просмотр справки
mh-compressor-manager --help
```

---

## 🔧 Управление службой

### Systemd команды

```bash
# Запуск службы
sudo systemctl start mh-compressor-manager

# Остановка службы
sudo systemctl stop mh-compressor-manager

# Перезапуск службы
sudo systemctl restart mh-compressor-manager

# Проверка статуса
sudo systemctl status mh-compressor-manager

# Включение автозапуска
sudo systemctl enable mh-compressor-manager

# Отключение автозапуска
sudo systemctl disable mh-compressor-manager
```

### Просмотр логов

```bash
# Логи в реальном времени
sudo journalctl -u mh-compressor-manager -f

# Логи за последний час
sudo journalctl -u mh-compressor-manager --since "1 hour ago"

# Только ошибки
sudo journalctl -u mh-compressor-manager -p err

# С временными метками
sudo journalctl -u mh-compressor-manager -o short-iso
```

---

## 📊 Примеры использования

### Базовый сценарий

```bash
# 1. Установка
sudo dnf install mh-compressor-manager-1.0.0-1.fc43.x86_64.rpm
sudo systemctl daemon-reload

# 2. Запуск службы
sudo systemctl start mh-compressor-manager

# 3. Создание тестового файла
echo "console.log('test');" | sudo tee /var/www/html/test.js

# 4. Проверка сжатых файлов (через 2 секунды)
ls -la /var/www/html/test.js*
# Ожидаемый результат: test.js, test.js.gz, test.js.br
```

### Проверка удаления

```bash
# Удаление исходного файла
sudo rm /var/www/html/test.js

# Сжатые копии должны быть удалены автоматически
ls -la /var/www/html/test.js*  # Файлы .gz и .br должны отсутствовать
```

---

## 🐛 Решение проблем

### Служба не запускается

```bash
# Проверка статуса
sudo systemctl status mh-compressor-manager

# Просмотр логов
sudo journalctl -u mh-compressor-manager -p err

# Проверка конфигурации
sudo mh-compressor-manager --config /etc/mediahive/compressor-manager.conf --dry-run
```

### Файлы не сжимаются

- ✅ Проверьте права доступа к целевой директории
- ✅ Убедитесь, что расширение файла есть в списке `list`
- ✅ Проверьте логи на наличие ошибок
- ✅ Запустите в режиме `--debug` для диагностики

### Превышение лимита inotify

```bash
# Проверка текущего лимита
cat /proc/sys/fs/inotify/max_user_watches

# Увеличение лимита
sudo sysctl fs.inotify.max_user_watches=524288
echo "fs.inotify.max_user_watches=524288" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p
```

### Ошибки сборки на Fedora 43

```bash
# Если возникает ошибка «zlib-devel not found»
sudo dnf install zlib-ng-compat-devel
```

---

## 🔒 Безопасность

### Права доступа

| Файл | Права | Владелец |
|------|-------|----------|
| Конфигурация | `644` | `root:root` |
| Бинарный файл | `755` | `root:root` |
| Systemd-юнит | `644` | `root:root` |

### Защита от уязвимостей

- ✅ Запрещено выполнение внешних команд (`system()`, `popen()`)
- ✅ Игнорирование символических ссылок (защита от symlink-атак)
- ✅ Использование `lchown()` вместо `chown()` для безопасности
- ✅ Проверка прав доступа перед операциями записи

### SELinux

```bash
# Проверка контекста файла
ls -Z /var/www/html/file.js

# Просмотр логов SELinux
sudo ausearch -m avc -ts recent

# Временное отключение (для тестирования)
sudo setenforce 0
```

---

## 📈 Производительность

### Ресурсные ограничения

| Ресурс | Лимит |
|--------|-------|
| **ОЗУ (покой)** | ≤ 50 МБ |
| **ОЗУ (пик)** | ≤ 200 МБ |
| **inotify события** | До 1000 событий/сек |
| **Потоки** | Настраивается (параметр `threads`) |

### Мониторинг ресурсов

```bash
# Потребление памяти
systemctl status mh-compressor-manager

# Использование CPU
top -p $(pgrep mh-compressor-manager)

# Файловые дескрипторы
ls -la /proc/$(pgrep mh-compressor-manager)/fd | wc -l
```

---

## 📁 Структура проекта

```
mh-compressor-manager/
├── src/                          # Исходный код
│   ├── CMakeLists.txt            # Конфигурация сборки
│   ├── main.cpp                  # Точка входа
│   ├── config.cpp/h              # Парсинг конфигурации
│   ├── compressor.cpp/h          # Алгоритмы сжатия
│   ├── monitor.cpp/h             # Мониторинг inotify
│   ├── logger.cpp/h              # Система логирования
│   ├── threadpool.h              # Пул потоков
│   ├── compressor-manager.conf   # Пример конфигурации
│   └── mh-compressor-manager.service  # Systemd юнит
├── build-rpm.sh                  # Скрипт сборки RPM
├── mh-compressor-manager.spec    # SPEC файл для RPM
├── LICENSE                       # Лицензия MIT
├── README.md                     # Этот файл
└── TECHNICAL_SPECIFICATION.html  # Техническое задание
```

---

## 🤝 Вклад в проект

### Сообщение об ошибках

Пожалуйста, сообщайте об ошибках через [GitHub Issues](https://github.com/pavelvkovalenko/mh-compressor-manager/issues), указывая:

1. Версию программы
2. Версию ОС
3. Шаги для воспроизведения
4. Ожидаемое и фактическое поведение
5. Логи службы (`journalctl -u mh-compressor-manager`)

### Запросы на добавление функций

Перед созданием запроса на добавление новой функции:

1. Проверьте существующие [запросы](https://github.com/pavelvkovalenko/mh-compressor-manager/issues)
2. Опишите использование функции
3. Укажите преимущества для пользователей

### Pull Requests

1. Создайте форк репозитория
2. Создайте ветку для вашей функции (`git checkout -b feature/amazing-feature`)
3. Зафиксируйте изменения (`git commit -m 'Add amazing feature'`)
4. Отправьте в ветку (`git push origin feature/amazing-feature`)
5. Откройте Pull Request

---

## 📄 Лицензия

Этот проект распространяется под лицензией **MIT**. См. файл [LICENSE](LICENSE) для получения дополнительной информации.

```
MIT License

Copyright (c) 2026 MediaHive.ru

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 👤 Правообладатель

<div align="center">

### 📄 Информация о правообладателе

**© 2026 MediaHive.ru**

**Владелец:** ООО ОКБ "Улей"

**Автор:** Коваленко Павел

**Лицензия:** [MIT](LICENSE)

</div>

| Параметр | Значение |
|----------|----------|
| **Проект** | mh-compressor-manager |
| **Версия** | 1.0.0 |
| **Стандарт** | C++23 |
| **Платформа** | Linux (Fedora 43+, Fedora 38+, CentOS, Ubuntu) |
| **Компилятор** | GCC 13+ или Clang 16+ |
| **Система сборки** | CMake 3.20+ |
| **Год разработки** | 2026 |
| **Лицензия** | MIT |

---

## 📞 Контакты

| Тип | Информация |
|-----|------------|
| **Репозиторий** | [https://github.com/pavelvkovalenko/mh-compressor-manager.git](https://github.com/pavelvkovalenko/mh-compressor-manager.git) |
| **Issues** | [GitHub Issues](https://github.com/pavelvkovalenko/mh-compressor-manager/issues) |
| **Документация** | [README.md](README.md), [README.html](https://html-preview.github.io/?url=https://github.com/pavelvkovalenko/mh-compressor-manager/blob/main/README.html), [TECHNICAL_SPECIFICATION.html](https://html-preview.github.io/?url=https://github.com/pavelvkovalenko/mh-compressor-manager/blob/main/TECHNICAL_SPECIFICATION.html) |
| **Техническое задание** | ТЗ версия 2 |
| **Лицензия** | [MIT](LICENSE) |

---

<div align="center">

**mh-compressor-manager v1.0.0** | Документация соответствует ТЗ версия 2

© 2026 MediaHive.ru, владелец: ООО ОКБ "Улей", автор: Коваленко Павел. Лицензия MIT.

[🔗 GitHub Repository](https://github.com/pavelvkovalenko/mh-compressor-manager)

</div>
