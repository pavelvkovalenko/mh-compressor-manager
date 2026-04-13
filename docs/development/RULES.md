# Правила написания кода — mh-compressor-manager

## Язык и стандарт
- C++23
- Стандарт C++23 обязателен, откат на C++20 только если `std::format` недоступен

## Язык комментариев
- **Комментарии в коде** — на **русском языке**
- **Пользовательские сообщения** (логи, --help, диагностика) — по умолчанию на **английском языке**, с возможностью перевода через gettext (`.po/.mo` файлы)
- **Doxygen-комментарии** — на **русском языке**
- **Имена переменных и функций** — на **английском языке** (snake_case)

**Пример:**
```cpp
// ✅ ПРАВИЛЬНО: комментарий на русском, сообщение на английском с переводом
// Инициализация мониторинга файловой системы
Logger::info_fmt(_("Initializing inotify watch on %s"), path.c_str());

// ❌ НЕПРАВИЛЬНО: сообщение на русском напрямую (без gettext)
Logger::info("Инициализация мониторинга файловой системы");
```

## Именование
- Функции: `snake_case`
- Классы: `PascalCase`
- Члены-переменные: `m_` префикс + `snake_case`
- Константы: `UPPER_SNAKE_CASE`
- Макросы: `UPPER_SNAKE_CASE`

## Стиль кода
- Отступ: 4 пробела
- Фигурные скобки: открывающая на той же строке, закрывающая на новой
- Максимальная длина строки: 120 символов
- `const` корректность обязательна
- `mutable` использовать только для `m_mutex` и кэшей в const методах

## Комментарии
- Комментарии на русском языке
- Документировать сложные алгоритмы и бизнес-логику
- Не комментировать очевидный код
- Doxygen-стиль для публичных API (`/** ... */`)

## Безопасность
- Запрещено: `sprintf`, `strcpy`, `gets` — использовать безопасные аналоги
- Все файловые операции через `openat` с `O_NOFOLLOW` для защиты от symlink-атак
- Проверка путей через `lstat`, не `stat`
- `validate_filename()` для проверки имён файлов

## Обработка ошибок
- Все ошибки логировать через `Logger::error/warning/info`
- `errno` сохранять немедленно: `int err = errno;`
- Частичные файлы удалять при ошибке: `unlink(output.c_str());`
- Не использовать исключения для потока управления — возвращать `false`

## Многопоточность
- `std::shared_lock` для чтения, `std::unique_lock` для записи
- Не вызывать `const_cast` — использовать `mutable`
- Не создавать вложенные захваты одного и того же `std::mutex`
- Detached потоков избегать — использовать joinable

## Управление ресурсами (RAII и умные указатели)
- **Максимально использовать умные указатели** (`std::unique_ptr`, `std::shared_ptr`) вместо сырых указателей (`new`/`delete`)
- **Обоснование:**
  - **Автоматическое освобождение:** Умные указатели гарантируют освобождение ресурсов при выходе из scope, даже при исключениях или раннем return
  - **Предотвращение утечек:** В коде с множеством точек возврата (error handling) ручной `delete`/`free` легко пропустить — умные указатели исключают эту проблему
  - **Явное владение:** `std::unique_ptr` явно выражает семантику исключительного владения, `std::shared_ptr` — совместного
  - **Кастомные deleter:** Для C API (zlib, libbrotli, SELinux) можно использовать custom deleter для автоматического вызова `deflateEnd()`, `BrotliEncoderDestroyInstance()`, `freecon()` и т.д.
  - **Нулевые накладные расходы:** `std::unique_ptr` имеет те же накладные расходы что и raw pointer — нет penalty за безопасность
  - **Совместимость с C API:** `.get()` передаёт raw pointer в C API когда требуется
- **Примеры:**
```cpp
// ✅ ПРАВИЛЬНО: unique_ptr с custom deleter для zlib
struct ZStreamDeleter { void operator()(z_stream* p) { if (p) { deflateEnd(p); delete p; } } };
struct GzipStreamState {
    std::unique_ptr<z_stream, ZStreamDeleter> strm;  // автоматический cleanup
};

// ✅ ПРАВИЛЬНО: unique_ptr для Brotli encoder
struct BrotliEncoderDeleter { void operator()(BrotliEncoderState* p) { if (p) BrotliEncoderDestroyInstance(p); } };
struct BrotliStreamState {
    std::unique_ptr<BrotliEncoderState, BrotliEncoderDeleter> enc;
};

// ✅ ПРАВИЛЬНО: raw pointer через .get() для C API
deflate(state.strm.get(), Z_FINISH);  // zlib C API требует raw pointer

// ❌ НЕПРАВИЛЬНО: raw pointer с ручным управлением
struct GzipStreamState {
    z_stream* strm;  // требует ручного deflateEnd + delete
};
```
- **Исключения:** Raw pointers допустимы при работе с пулами памяти (`memory_pool.h`) где контроль за выделением/освобождением намеренно централизован
