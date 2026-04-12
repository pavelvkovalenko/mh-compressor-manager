# Аудит — mh-compressor-manager

> **ℹ️ Примечание:** Данный каталог содержит все материалы, связанные с аудитом качества кода: процедуры, историю раундов, changelog и служебные скрипты.

---

## 📑 Содержание

### 🔄 Процедуры аудита

| Документ | Описание |
|----------|----------|
| [**AUDIT_CYCLE.md**](AUDIT_CYCLE.md) | Процедура стандартного цикла аудита (10-20 мин) |
| [**AUDIT_FULL_CYCLE.md**](AUDIT_FULL_CYCLE.md) | Процедура полного аудита с субагентами (30-60 мин) |
| [**CHECK_DOCUMENTATION.md**](CHECK_DOCUMENTATION.md) | Процедура «Проверь документацию» — проверка актуальности ТЗ перед действием |

### 📊 История и отчёты

| Документ | Описание |
|----------|----------|
| [**CHANGELOG.md**](CHANGELOG.md) | Журнал всех изменений кода по раундам аудита |
| [**ROUND_HISTORY.md**](ROUND_HISTORY.md) | История раундов: что найдено, что исправлено, статистика |

### 🧹 Служебные материалы

| Документ | Описание |
|----------|----------|
| [**CLEANUP_DEAD_CODE.md**](CLEANUP_DEAD_CODE.md) | Отчёт об очистке мёртвого кода и неиспользуемых зависимостей |
| [_pr_body.md](_pr_body.md) | Черновик тела Pull Request (служебный) |

### 🛠️ Скрипты

| Скрипт | Описание |
|--------|----------|
| [**check_context_changes.sh**](check_context_changes.sh) | Проверка изменений документации (Linux/macOS) |
| [**check_context_changes.bat**](check_context_changes.bat) | Проверка изменений документации (Windows) |

---

## 🔄 Как проводится аудит

### Стандартный цикл (AUDIT_CYCLE.md)

```
1. Запуск набора тестов
2. Фиксация найденных ошибок
3. Приоритизация (critical → high → medium → low)
4. Исправление одной ошибки
5. Повторный запуск тестов
6. Если есть ошибки → возврат к шагу 2
7. Если ошибок нет → аудит завершён
```

### Полный аудит (AUDIT_FULL_CYCLE.md)

```
Шаг 0:  Pre-flight (проверка сборки, история)
Шаг 1:  Загрузка полного контекста (Master)
Шаг 2:  Параллельный анализ субагентов (A-F)
Шаг 3:  Верификация Master-ом
Шаг 4:  Последовательное исправление
Шаг 5:  Автоматизированные проверки (RULES.md, static analysis)
Шаг 6:  Edge-Case и Stress-проверки
Шаг 7:  Security: CWE/OWASP Checklist
Шаг 8:  Configuration Matrix Testing
Шаг 9:  Сборка и тестирование
Шаг 10: Regression Test Auto-Generation
Шаг 11: Automated Changelog Update
Шаг 12: Коммит и пуш
Шаг 13: Отчёт и публикация
```

### Субагенты полного аудита

| Субагент | Область | Что ищет |
|----------|---------|----------|
| **A** | Сжатие, I/O, память | Утечки, streaming, one-shot, API, атомарность |
| **B** | Мониторинг, потоки, конфиг | Race conditions, deadlock, debounce, inotify |
| **C** | I/O, инфраструктура | async I/O, pipeline, config parsing, logger |
| **D** | Оптимизации | libdeflate, CPU affinity, cache-aware, NUMA |
| **E** | Безопасность | CWE/OWASP, symlink-атаки, TOCTOU, seccomp |
| **F** | Документация, тесты | Соответствие ТЗ, работоспособность тестов |

---

## 🏗️ Структура каталога

```
audit/
├── README.md                    ← Этот файл (оглавление)
├── AUDIT_CYCLE.md               ← Процедура стандартного цикла аудита
├── AUDIT_FULL_CYCLE.md          ← Процедура полного аудита с субагентами
├── CHECK_DOCUMENTATION.md       ← Процедура проверки актуальности документации
├── CHANGELOG.md                 ← Журнал всех изменений по раундам
├── ROUND_HISTORY.md             ← История раундов аудита (статистика)
├── CLEANUP_DEAD_CODE.md         ← Отчёт об очистке мёртвого кода
├── check_context_changes.sh     ← Скрипт проверки изменений (Linux/macOS)
├── check_context_changes.bat    ← Скрипт проверки изменений (Windows)
└── _pr_body.md                  ← Черновик тела Pull Request (служебный)
```

---

## 📊 Сводная статистика аудита

| Метрика | Значение |
|---------|----------|
| **Всего раундов** | 8 (раунд 8 — полный аудит, 2026-04-12) |
| **Всего исправлений** | ~42 |
| **Критических исправлений** | ~15 |
| **Раундов без ошибок** | 1 (раунд 5) |

Подробная статистика: [ROUND_HISTORY.md](ROUND_HISTORY.md)

---

## 🔗 Быстрые ссылки

| Ресурс | Ссылка |
|--------|--------|
| **Техническая спецификация** | [../docs/specification/TECHNICAL_SPECIFICATION.md](../docs/specification/TECHNICAL_SPECIFICATION.md) |
| **Правила кода** | [../docs/development/RULES.md](../docs/development/RULES.md) |
| **Сборка и деплой** | [../docs/development/DEPLOY.md](../docs/development/DEPLOY.md) |
| **Тесты** | [../tests/TEST_SCRIPTS.md](../tests/TEST_SCRIPTS.md) |
| **Контекст AI** | [../docs/development/QWEN.md](../docs/development/QWEN.md) |
| **Pull Request** | [../CONTRIBUTING.md](../CONTRIBUTING.md) |

---

**© 2026 MediaHive.ru** | ООО ОКБ "Улей" | Автор: Коваленко Павел
