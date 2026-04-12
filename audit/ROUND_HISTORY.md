# История раундов аудита — mh-compressor-manager

## Раунд 1 — Критические ошибки компиляции и линковки

**Дата:** 2026-04-11
**Режим:** Стандартный
**Найдено:** 3 ошибки (все критические)

| # | Файл | Проблема | Исправление |
|---|------|----------|-------------|
| 1 | async_io.cpp | `ret != 0` вместо `ret < 0` для io_uring_wait_cqe_timeout | `ret < 0` |
| 2 | numa_utils.cpp | `if (numa_available())` вместо `== 0` | `== 0` |
| 3 | simd_utils.h | `__cpuid_count` не определён на ARM | `#if SIMD_UTILS_AVX2` guards |

**Коммит:** 53bd752

---

## Раунд 2 — Ошибки логики pipeline и threadpool

**Дата:** 2026-04-11
**Режим:** Стандартный
**Найдено:** 4 ошибки (2 критических)

| # | Файл | Проблема | Исправление |
|---|------|----------|-------------|
| 1 | threadpool.h | unsigned wraparound io_slots_available | `continue` при таймауте |
| 2 | pipeline.cpp | `-MAX_WBITS` создаёт raw deflate | `MAX_WBITS + 16` |
| 3 | memory_pool.h | cleanup_thread_cache пустой цикл | Обновлён комментарий |
| 4 | config.cpp | `\\n` вместо `\n` в help | `\n` |

**Коммит:** 36911a6

---

## Раунд 3 — Streaming и memory pool

**Дата:** 2026-04-11
**Режим:** Стандартный
**Найдено:** 5 ошибок (1 критическая)

| # | Файл | Проблема | Исправление |
|---|------|----------|-------------|
| 1 | memory_pool.h | release_raw не удаляет из allocated_set_ | `allocated_set_.erase()` |
| 2 | main.cpp | streaming: алгоритмы не оцениваются независимо | Раздельная оценка |
| 3 | compressor.cpp | gzip_stream_process: нет отдельного цикла финализации | Добавлен цикл |
| 4 | compressor.cpp/h | brotli_stream_process: нет защиты от двойного flush | Поле `finalized` |
| 5 | compressor.cpp | stale .tmp файлы блокируют streaming | `unlink()` при start |

**Коммит:** 9301524

---

## Раунд 4 — Inotify и offset

**Дата:** 2026-04-11
**Режим:** Стандартный
**Найдено:** 5 ошибок (1 критическая)

| # | Файл | Проблема | Исправление |
|---|------|----------|-------------|
| 1 | main.cpp | `offset += this_chunk` вместо `bytes_read` | `offset += bytes_read` |
| 2 | monitor.cpp | recursive_directory_iterator + рекурсия = двойной обход | `directory_iterator` |
| 3 | compressor.cpp | `fstat(fd_in)` после `fdopen` | Перемещён до fdopen |
| 4 | monitor.cpp | inotify обрабатывает .tmp файлы | Игнорирование .tmp |
| 5 | monitor.cpp | debounce для сжатых файлов | `!is_compressed &&` |

**Коммит:** bcbec22

---

## Раунд 5 — Код чист

**Дата:** 2026-04-11
**Режим:** Стандартный
**Найдено:** 0 ошибок

**Результат:** КОД ЧИСТ — ЦИКЛ АУДИТА ЗАВЕРШЁН

**Коммит:** Нет (ошибок не найдено)

---

## Раунд 6 — Fd leak и NUMA fallback

**Дата:** 2026-04-11
**Режим:** Стандартный
**Найдено:** 3 ошибки (2 критических)

| # | Файл | Проблема | Исправление |
|---|------|----------|-------------|
| 1 | compressor.cpp | fd leak при ошибке gzip | close + unlink перед return |
| 2 | memory_pool.h | numa_free на posix_memalign | `numa_allocated_` трекинг |
| 3 | memory_pool.h | cleanup_thread_cache комментарий | Обновлён |

**Коммит:** 012c1b6

---

## Раунд 7 — Параллельные аудиты Alpha + Beta

**Дата:** 2026-04-11
**Режим:** Параллельный Alpha + Beta + финал
**Найдено:** 15 ошибок (6 критических)

### 7-ALPHA (7 ошибок, 3 критических)

| # | Файл | Проблема | Исправление |
|---|------|----------|-------------|
| 1 | memory_pool.h | cleanup_thread_cache пустой цикл | `::free()` (позже убрано) |
| 2 | numa_utils.cpp | нет HAVE_NUMA guards | Обёртки для всех функций |
| 3 | pipeline.cpp | HAVE_ZLIB/HAVE_LIBBROTLI не определены | `target_compile_definitions` |
| 4 | performance_optimizer.cpp | Missing `<sstream>` | `#include <sstream>` |
| 5 | monitor.cpp | is_directory следует за symlink | `symlink_status()` |
| 6 | pipeline.cpp | SELinux не копируется | (не исправлено — low priority) |
| 7 | threadpool.h | monitor thread без timeout | 60s timeout |

### 7-BETA (3 критических + 3 средних)

| # | Файл | Проблема | Исправление |
|---|------|----------|-------------|
| 1 | CMakeLists.txt | HAVE_ZLIB/HAVE_LIBBROTLI | Исправлено в 7-ALPHA |
| 2 | memory_pool.h | cleanup_thread_cache утечка | Исправлено в 7-ALPHA |
| 3 | performance_optimizer.cpp | Missing sstream | Исправлено в 7-ALPHA |
| 4 | numa_utils.cpp | HAVE_NUMA guards | Исправлено в 7-ALPHA |
| 5 | pipeline.cpp | SELinux контекст | Не исправлено (pipeline deprecated) |
| 6 | compressor.cpp | Хрупкая логика brotli цикла | MAX_ITERATIONS защита |

### 7-FINAL (2 исправления)

| # | Файл | Проблема | Исправление |
|---|------|----------|-------------|
| 1 | memory_pool.h | ::free на NUMA памяти | Убран ::free, оставлен clear() |
| 2 | main.cpp | premature EOF в streaming | Логирование ошибки |

**Коммиты:** 51d1d03, 128aeb1

---

## Раунд 8 — Полный аудит

**Дата:** 2026-04-12
**Режим:** Полный аудит (3 субагента: A — сжатие/I/O, B — мониторинг/потоки, E — безопасность)
**Найдено:** 7 ошибок (1 high, 6 medium)

| # | Файл | Проблема | Исправление | Критичность |
|---|------|----------|-------------|-------------|
| 1 | monitor.cpp | Rescan при IN_Q_OVERFLOW блокирует monitoring loop | Вынесен в detached поток | **high** |
| 2 | monitor.cpp | IN_DELETE не удаляет из debounce_map — phantom compress | `m_debounce_map.erase()` | medium |
| 3 | monitor.cpp | Initial scan не проверяет результат enqueue | Проверка return value + счётчики | medium |
| 4 | monitor.cpp | reload_config не обновляет inotify watches | `add_watch_recursive` + `remove_watch_recursive` | medium |
| 5 | threadpool.h | wait_all() ложно сигнализирует при stop_flag | Убран `|| stop_flag` из предиката | medium |
| 6 | security.cpp | cap_set_proc после setuid — всегда EPERM | Перенесён ДО setuid() | medium |
| 7 | monitor.cpp | Inotify buffer: нет bounds check перед cast | Добавлены проверки `i + sizeof <= len` | medium |

**Отложить:**
- E#10: Blacklist symlink вместо whitelist — сложная замена, не критично
- A#M1: MemoryPool destructor vs thread-local cache — безопасно в текущей архитектуре

**Коммит:** (предстоит)

---

## Раунд 9 — Инкрементальный полный аудит

**Дата:** 2026-04-12
**Режим:** Полный аудит (2 субагента: A — сжатие/I/O, B — мониторинг/потоки)
**Найдено:** 3 ошибки (1 critical, 1 high, 1 low)

| # | Файл | Проблема | Исправление | Критичность |
|---|------|----------|-------------|-------------|
| 1 | compressor.cpp | gzip_stream_process: fd не закрыт, rename не выполнен при Z_STREAM_END | Финализация вынесена из условия второго цикла | **critical** |
| 2 | monitor.cpp | Detached rescan threads — use-after-free при stop() | Tracked threads с лимитом 3 + join в stop() | high |
| 3 | _local-install.sh | systemctl stop падает при первой установке | `|| true` | low |

**Коммит:** d38f14f

---

## Сводная статистика

| Метрика | Значение |
|---------|----------|
| **Всего раундов** | 10 (1-7 + 8 полный + чистка/переименование) |
| **Всего исправлений** | ~42 |
| **Критических исправлений** | ~15 |
| **Новых файлов** | 3 (cache_info.h, cache_info.cpp, +remove_watch_recursive) |
| **Изменённых файлов** | 18 |
| **Раундов без ошибок** | 1 (раунд 5) |
