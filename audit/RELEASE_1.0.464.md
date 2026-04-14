# Release Notes: mh-compressor-manager v1.0.464

**Дата релиза:** 14 апреля 2026
**Предыдущая версия:** 1.0.88 (11 апреля 2026)
**Коммитов с предыдущего релиза:** 46

---

## 🚀 Новые возможности

### Локализация (i18n)
- Полный русский перевод всех пользовательских сообщений (292 строки)
- Перевод `--help` и `--version` на русский язык через gettext
- Установка `.mo` файлов в `/usr/share/locale/ru/LC_MESSAGES/`
- Поддержка fallback на английский при отсутствии перевода

### Man-страницы
- Английская man-страница (`man/en/man1/mh-compressor-manager.1`)
- Русская man-страница (`man/ru/man1/mh-compressor-manager.1`)
- Раздел «Управление через systemd» с примерами `systemctl` и `journalctl`
- Ссылки на GitHub-репозиторий и профиль автора
- Установка в `/usr/share/man/man1/` и `/usr/share/man/ru/man1/`

### Bash completion
- Автодополнение всех `--опций` при нажатии `Tab`
- Контекстное дополнение: `--config` → файлы, `--dir` → директории
- Числовые подсказки: `--gzip-level` (1-9), `--brotli-level` (1-11)
- Установка в `/usr/share/bash-completion/completions/`

### Версия в CMake Build Configuration
- Строка `Version: X.Y.Z` выводится при сборке CMake

---

## 🔒 Критические исправления безопасности

### CAP_DAC_OVERRIDE терялся после сброса привилегий
- **Проблема:** `cap_set_proc()` вызывался ДО `setuid()`, но без `prctl(PR_SET_KEEPCAPS, 1)`. Ядро очищало effective capabilities при `setuid()`, процесс терял доступ к файлам без owner-read прав.
- **Исправление:** Добавлен `prctl(PR_SET_KEEPCAPS, 1)` перед `cap_set_proc()`.
- **Файл:** `src/security.cpp`

### Race condition с nanosecond precision
- **Проблема:** Сравнение `st_mtime` (секунды) пропускало изменения файла в пределах одной секунды на файловых системах с наносекундной точностью (ext4, xfs).
- **Исправление:** Теперь сравниваются `st_mtim.tv_sec` И `st_mtim.tv_nsec` на Linux.
- **Файл:** `src/main.cpp` (2 места: one-shot и streaming)

### Права файлов зависели от umask
- **Проблема:** `write_atomic_file()` не вызывал `fchmod()`, права задавались только через `open(..., mode)`, который зависит от umask процесса.
- **Исправление:** Добавлен явный `fchmod(fd, mode)` перед `fsync()`.
- **Файл:** `src/compressor.cpp`

---

## ⚡ Оптимизации производительности

### _mm_prefetch для ARM-совместимости
- `_mm_prefetch` обёрнут в `#ifdef __x86_64__` для сборки на ARM (aarch64)
- `<immintrin.h>` включается только на x86_64
- **Файл:** `src/memory_pool.h`

### Brotli encoder RAII
- `BrotliEncoderState` теперь управляется через `std::unique_ptr` с custom deleter
- Устранена утечка при раннем `return` из `compress_brotli_from_memory()`
- **Файл:** `src/compressor.cpp`

### NUMA nullptr check
- Добавлена проверка `numa_all_nodes_ptr` на nullptr перед `numa_set_interleave_mask()`
- **Файл:** `src/numa_utils.cpp`

---

## 🛠️ Сборка и инфраструктура

### RPM-пакет
- Версия RPM теперь берётся из `CMakeLists.txt` (не из SPEC)
- Tarball содержит полную структуру проекта: `src/`, `man/`, `completion/`, `translations/`
- Удалён несуществующий `README.html` из `%doc`
- Добавлены man-страницы, bash completion, locale в `%files`
- Обновлён changelog в SPEC

### CI/CD и документация
- Правило: автоматический подсчёт коммитов перед пушем (`PROJECT_VERSION_PATCH`)
- Правило: актуализация спецификации при обновлении версии
- Правило: обновление SPEC при изменении версии
- Правило: проверка ссылок при изменении документации
- Правило: обязательная проверка переводов в режиме полного аудита (Режим 3)
- Полный аудит документации (Режим 3): код + тесты + документация

### Исправления документации
- Исправлены битые ссылки в содержании ТЗ (разделы 3.2.1-3.2.9, 18.6.5/18.6.6)
- Убран дубликат `--min-compress-size` (алиас `--min-size`)
- Синхронизированы версии во всех файлах (CMakeLists.txt, README, SPEC, TECH_SPEC, ru.po)
- Обновлён TECHNICAL_SPECIFICATION.html
- Добавлены подразделы раздела 6 (CLI, man, completion) в содержание ТЗ

### Мёртвый код
- Удалено неиспользуемое поле `l3_shared_cores` из `cache_info.h/cpp`
- Удалены дублирующиеся `#include <cerrno>` и `<algorithm>` в `compressor.cpp`
- Удалены дубликаты `const char*` в streaming функциях (`std::span` вместо raw ptr+size)

---

## 📊 Статистика релиза

| Метрика | Значение |
|---------|----------|
| Коммитов | 46 |
| Файлов изменено | 40+ |
| Строк добавлено | ~1500 |
| Строк удалено | ~200 |
| Критических исправлений | 4 |
| Исправлений безопасности | 3 |
| Оптимизаций | 3 |
| Новых файлов документации | 4 (man en/ru, completion, release notes) |

---

## ✅ Критерии приёмки

- [x] Сборка без ошибок (GCC 15.2.1, Fedora 43)
- [x] Сборка без предупреждений
- [x] Сервис `active (running)` после установки
- [x] Seccomp sandbox инициализирован и активен
- [x] Capabilities установлены корректно (CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH)
- [x] Русский перевод работает (`--help` на русском)
- [x] Man-страница доступна (`man mh-compressor-manager`)
- [x] Bash completion установлен
- [x] RPM-пакет собирается и устанавливается
- [x] Все версии синхронизированы (CMakeLists, README, SPEC, TECH_SPEC, ru.po)
- [x] CHANGELOG обновлён

---

**© 2026 MediaHive.ru** | ООО ОКБ "Улей" | Автор: Коваленко Павел
