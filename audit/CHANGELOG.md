# Changelog — mh-compressor-manager

## [Раунд 12] — 2026-04-13 — Полный аудит с тестированием

### Критические исправления
- **compressor.cpp**: Brotli one-shot финализация в цикле `while(!BrotliEncoderIsFinished)` — один вызов `BrotliEncoderCompressStream` мог не финализировать все данные, приводя к неполным `.br` файлам

### Исправления логики
- **compressor.cpp**: `stream_error` в streaming теперь удаляет полузаписанные `.gz`/`.br` через `safe_remove_compressed()` — предотвращение orphan-файлов при ошибке чтения чанка
- **main.cpp**: partial read в one-shot ветке теперь отменяет сжатие и удаляет stale-копии — если файл усечён во время чтения (race condition), результат отбрасывается

### Документация
- **memory_pool.h**: добавлена документация `cleanup_thread_cache()` для NUMA-систем

## [Раунд 11] — 2026-04-12 — Инкрементальный аудит (синхронизация)

### Критические исправления
- **compressor.cpp**: `libdeflate_gzip_compress_bound(nullptr, size)` → `libdeflate_gzip_compress_bound(comp, size)` — UB при передаче nullptr в libdeflate API

### Исправления безопасности
- **compressor.cpp**: `is_symlink_attack` — blacklist системных путей заменён на полный запрет ВСЕХ symlink (CWE-22 Path Traversal)
- **security.cpp**: `cap_init()` returning NULL теперь вызывает `return false` вместо продолжения без capabilities (CWE-276)
- **security.cpp**: хардкод syscall 291 заменён на `__NR_futimens` для кросс-архитектурной совместимости
- **numa_utils.cpp**: `stat()` → `lstat()` — защита от symlink-following (CWE-22)

### Исправления компиляции
- **i18n.h**: добавлены `<clocale>` и `<libintl.h>` (условно при HAVE_GETTEXT)
- **async_io.cpp**: удалена неиспользуемая `g_pending_submissions`

### Локализация
- **logger.h/cpp**: перегрузка вместо `*_fmt` методов — единый API `Logger::info(str)` и `Logger::info(fmt, ...)`
- Все `*_fmt` вызовы заменены на `*` (автоматический выбор перегрузки)

## [Раунд 9] — 2026-04-12 — Инкрементальный полный аудит

### Критические исправления
- **compressor.cpp**: `gzip_stream_process` — fsync/close/rename были внутри `if (last_ret != Z_STREAM_END)` — при нормальном завершении deflate в первом цикле файл НЕ переименовывался, fd НЕ закрывался, `.gz.tmp` удалялся деструктором. Streaming gzip **не работал вообще**. Исправлено: финализация вынесена за пределы условия второго цикла.

### Исправления логики
- **monitor.cpp**: detached rescan-потоки при inotify overflow заменены на tracked (`m_rescan_threads`) с лимитом 3 и join в `stop()` — устранение use-after-free при уничтожении Monitor
- **_local-install.sh**: `systemctl stop` с `|| true` — скрипт не падает при первой установке (сервис ещё не существует)

### Улучшения
- **_rpm-build.sh**: интерактивная установка зависимостей (`y/n`) + флаг `--yes` для CI

## [Раунд 8] — 2026-04-12 — Полный аудит

### Исправления логики мониторинга
- **monitor.cpp**: Rescan при `IN_Q_OVERFLOW` вынесен в detached поток — основной monitoring loop больше не блокируется
- **monitor.cpp**: При `IN_DELETE` запись удаляется из `m_debounce_map` — предотвращение phantom compression после удаления файла
- **monitor.cpp**: `scan_existing_files` проверяет возвращаемое значение `m_on_compress` (enqueue result) — при переполнении очереди логируется WARNING, файлы не теряются молча
- **monitor.cpp**: `reload_config` теперь обновляет inotify watches — добавлены `remove_watch_recursive` для старых путей и `add_watch_recursive` для новых
- **monitor.cpp**: Добавлена проверка bounds буфера перед cast `inotify_event` — предотвращение out-of-bounds read при повреждённом буфере

### Исправления многопоточности
- **threadpool.h**: `wait_all()` больше не прерывается по `stop_flag` — гарантирует завершение всех поставленных задач

### Исправления безопасности
- **security.cpp**: `cap_set_proc` перенесён ДО `setuid()` — ранее вызов после setuid возвращал EPERM silently, capabilities не устанавливались
- **monitor.cpp**: Bounds check для inotify buffer — проверка `i + sizeof(inotify_event) <= len` перед cast указателя

## [1.0.88] — 2026-04-11

### Исправления безопасности
- **memory_pool.h**: Добавлен трекинг NUMA-выделенной памяти (`numa_allocated_`) для предотвращения UB при `numa_free()` на памяти от `posix_memalign()`
- **monitor.cpp**: Замена `entry.is_directory()` на `entry.symlink_status()` для предотвращения следования за symlink при обходе директорий
- **monitor.cpp**: Игнорирование `.tmp` файлов сжатия в inotify обработчике
- **monitor.cpp**: `!is_compressed &&` для debounce — предотвращение race condition при streaming сжатии

### Исправления корректности
- **compressor.cpp**: При ошибке записи gzip fd_out закрывается и partial файл удаляется — предотвращение stale `.gz` файлов
- **compressor.cpp**: `fstat(fd_in)` перемещён ДО `fdopen(fd_in)` — устранение неопределённого поведения POSIX
- **compressor.cpp**: Добавлен отдельный цикл финализации `Z_FINISH` в `gzip_stream_process` с проверкой `last_ret != Z_STREAM_END`
- **compressor.cpp/h**: Защита от двойного `flush` в `brotli_stream_process` через поле `finalized`
- **compressor.cpp**: `unlink()` stale `.tmp` файлов при `gzip_stream_start`/`brotli_stream_start`
- **main.cpp**: `offset += bytes_read` вместо `offset += this_chunk` в streaming цикле
- **main.cpp**: Проверка premature EOF в streaming цикле
- **main.cpp**: Каждый алгоритм streaming оценивается независимо (gzip не блокируется ошибкой brotli)

### Исправления компиляции
- **CMakeLists.txt**: Добавлены `HAVE_ZLIB`/`HAVE_LIBBROTLI` макросы для pipeline.cpp
- **CMakeLists.txt**: zlib линкуется всегда (`-lz`) даже при наличии libdeflate
- **numa_utils.cpp**: Все функции обёрнуты в `#if HAVE_NUMA` — корректная компиляция без libnuma
- **performance_optimizer.cpp**: Добавлен `#include <sstream>` для `std::istringstream`
- **pipeline.cpp**: `-MAX_WBITS` заменён на `MAX_WBITS + 16` для корректного gzip формата

### Исправления логики
- **async_io.cpp**: `ret != 0` → `ret < 0` для `io_uring_wait_cqe_timeout` (2 места)
- **numa_utils.cpp**: `if (numa_available())` → `if (numa_available() == 0)`
- **threadpool.h**: Циклическое распределение CPU affinity (`i % cpu_count`) при `threads > cores`
- **threadpool.h**: `continue` при таймауте I/O слота вместо wraparound
- **threadpool.h**: Monitor thread timeout 60 секунд
- **config.cpp**: `\\n` → `\n` в help-сообщении `--debug`
- **memory_pool.h**: `release_raw()` теперь удаляет из `allocated_set_` при помещении в thread_local кэш
- **simd_utils.h**: `#if SIMD_UTILS_AVX2` guards для ARM/не-x86 совместимости

### Оптимизации сжатия (ТЗ §3.2.4–3.2.9)
- **Однократное чтение**: Файл читается 1 раз, оба алгоритма сжимают из одного буфера
- **libdeflate**: 2x ускорение gzip (600 vs 300 МБ/сек), fallback на zlib
- **Streaming чанки**: Файлы > 256 КБ сжимаются чанками с адаптацией под L3 кэш CPU
- **CacheInfo**: Определение L3 через sysfs, адаптивный размер буфера
- **Новые файлы**: `cache_info.h`, `cache_info.cpp`

### Обновления документации
- **TECHNICAL_SPECIFICATION.md**: v3.4, добавлен раздел 21 «План реализации оптимизаций сжатия»
- **README.md**: Добавлена строка об оптимизациях сжатия
- **CONTRIBUTING.md**: Добавлен пример коммита для текущей функциональности

---

## Статистика аудита

| Раунд | Дата | Исправлений | Критических | Режим |
|-------|------|------------|-------------|-------|
| **1** | 2026-04-11 | 3 | 3 | Стандартный |
| **2** | 2026-04-11 | 4 | 2 | Стандартный |
| **3** | 2026-04-11 | 5 | 1 | Стандартный |
| **4** | 2026-04-11 | 5 | 1 | Стандартный |
| **5** | 2026-04-11 | 0 | 0 | Стандартный (чисто) |
| **6** | 2026-04-11 | 3 | 2 | Стандартный |
| **7-A** | 2026-04-11 | 7 | 3 | Параллельный Alpha |
| **7-B** | 2026-04-11 | 6 | 3 | Параллельный Beta |
| **7-final** | 2026-04-11 | 2 | 0 | Финальные исправления |
| **ИТОГО** | | **35** | **15** | |
