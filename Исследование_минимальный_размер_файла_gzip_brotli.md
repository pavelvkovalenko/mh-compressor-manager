# Исследование: минимальный размер файла для сжатия gzip/brotli

**Дата:** 11 апреля 2026 г.  
**Проект:** mh-compressor-manager  
**Цель:** Определение минимального порога размера файлов для предварительного сжатия gzip/brotli

---

## 📋 Аннотация

Данное исследование посвящено определению оптимального минимального размера файла, при котором сжатие gzip/brotli становится целесообразным для веб-сервера. Основная проблема заключается в том, что накладные расходы на распаковку файлов малого размера могут превышать выгоду от снижения сетевого трафика.

**Ключевой вывод:** Файлы размером менее 150 байт после сжатия gzip могут стать **больше оригинала** из-за служебных заголовков и словаря сжатия.

---

## 📊 Консенсус индустрии

| Источник | Рекомендуемый минимум | Обоснование |
|----------|----------------------|-------------|
| **Google** | 150–1000 байт | Файлы <150 байт могут увеличиться после сжатия |
| **Akamai (CDN)** | 860 байт | Файлы до 860 байт помещаются в один TCP-пакет, сжатие не даёт сетевых преимуществ |
| **GetPageSpeed** | 256 байт | Практический оптимум для веб-серверов, рекомендован для nginx |
| **Apache Tomcat** | 2 КБ | Консервативный подход для серверов приложений |
| **CubePath** | 1 КБ | Накладные расходы не окупаются для файлов <1KB |
| **OneUptime** | 1 КБ (Brotli) | Пропуск файлов <1KB для Brotli-сжатия |
| **StackExchange (сообщество)** | 150–1000 байт | На основе практического тестирования |

---

## 🔍 Технические накладные расходы

### GZIP overhead

- **Заголовок:** 10 байт
- **Футер (CRC32 + размер):** 8 байт
- **Итого служебных данных:** 18 байт
- **Словари Хаффмана для малых файлов:** ~50–65 байт
- **Общий overhead:** 18–83 байта в зависимости от содержимого

### Риск увеличения файла

- Файлы **<150 байт** после сжатия gzip могут стать **больше оригинала**
- **Причина:** служебные заголовки + словарь сжатия превышают выигрыш от алгоритма DEFLATE
- **Механизм предотвращения:** DEFLATE автоматически формирует блоки типа 0 (Uncompressed), когда сжатие не даёт выгоды, сохраняя полезную нагрузку в исходном виде

### Сетевые соображения

- **Один TCP-пакет:** файлы до ~860 байт помещаются в один пакет (MTU ~1500 байт с учётом заголовков)
- **Вывод Akamai:** сжатие файлов, помещающихся в один пакет, не даёт заметных сетевых преимуществ

---

## 📈 Практические результаты тестирования

### Тест 1: пользователь Yura (StackExchange)
- **Метод:** Проверка порога 2 КБ
- **Результат:** Выходной размер файла практически не уменьшается, добавляются временные затраты на сжатие
- **Рекомендация:** Поднять порог до 3–4 КБ

### Тест 2: пользователь utt73 (StackExchange)
- **Порог:** ~350 байт
- **Результат:** Сжатие AJAX-запросов в диапазоне 350–1000 байт
- **Вывод:** Прямой выигрыш в производительности минимален, но снижаются затраты на CDN
- **Итог:** В сочетании с кэшированием достигнуты показатели загрузки страницы и Time to Interactive (TTI) < 2 секунд

### Тест 3: библиотека jQuery (GetPageSpeed)
- **Исходный размер:** 89 КБ
- **После Brotli:** 27 КБ
- **Экономия:** ~70% по сравнению с оригиналом, ~13% лучше gzip

---

## 🎯 Рекомендуемые пороги

| Размер файла | Стратегия | Обоснование |
|--------------|-----------|-------------|
| **<150 байт** | ❌ Не сжимать | Файлы могут увеличиться |
| **150–255 байт** | ⚠️ Пограничная зона | Выигрыш минимален (10–30 байт), распаковка добавляет задержку |
| **256–1023 байт** | ✅ Сжимать | Умеренный выигрыш, стабильное сжатие |
| **≥1024 байт (1 КБ)** | ✅✅ Обязательно | Значительный выигрыш, оптимальная зона |

---

## 💡 Практическая рекомендация для mh-compressor-manager

### Константы для реализации

```cpp
// Рекомендуемые константы
constexpr size_t MIN_COMPRESS_SIZE = 256;      // Минимальный порог сжатия
constexpr size_t OPTIMAL_COMPRESS_SIZE = 1024; // Оптимальный размер (1 КБ)
```

### Стратегия сжатия

- **Файлы <256 байт** — пропускать (не сжимать)
- **Файлы 256–1023 байт** — сжимать (умеренный выигрыш)
- **Файлы ≥1024 байт** — сжимать обязательно (значительный выигрыш)

### Почему именно 256 байт для mh-compressor-manager

✅ **Предварительное сжатие** (не на лету) — затраты на процессор не важны  
✅ **Максимальная степень сжатия** — brotli level 11 эффективнее даже для малых файлов  
✅ **Распаковка на клиенте** — дешёвая операция, но для файлов <256 байт время распаковки сравнимо с экономией трафика  
✅ **Сетевые пакеты** — файлы до ~860 байт помещаются в один TCP-пакет, но 256+ байт уже дают экономию 30–70%  
✅ **Совместимость с nginx** — согласуется с настройками по умолчанию (`gzip_min_length 256;`, `brotli_min_length 256;`)

---

## 📚 Источники

1. **GetPageSpeed** — NGINX Gzip Compression: Optimal Settings for Performance (2026)  
   https://www.getpagespeed.com/server-setup/nginx/nginx-gzip-compression

2. **StackExchange** — What is recommended minimum object size for gzip performance benefits  
   https://webmasters.stackexchange.com/questions/31750/what-is-recommended-minimum-object-size-for-gzip-performance-benefits

3. **Zuplo** — Implementing Data Compression in REST APIs with gzip and Brotli (2025)  
   https://zuplo.com/learning-center/implementing-data-compression-in-rest-apis-with-gzip-and-brotli/

4. **Koder.ai** — ZSTD vs Brotli vs GZIP: Choosing API Compression (2025)  
   https://koder.ai/blog/zstd-vs-brotli-vs-gzip-api-compression

5. **DEV Community** — Brotli vs. Gzip for Web Performance In Static Sites (2025)  
   https://dev.to/lovestaco/brotli-vs-gzip-for-web-performance-in-static-sites-2nhk

6. **StackOverflow** — How expensive is it to use compression & decompression on tiny payloads (2016)  
   https://stackoverflow.com/questions/40515628/how-expensive-is-it-to-use-compression-decompression-on-tiny-payloads-100byt

7. **Webhosting.de** — Gzip vs Brotli: HTTP compression methods compared (2026)  
   https://webhosting.de/en/gzip-vs-brotli-comparison-hosting-optimus/

8. **Kinsta** — Brotli Compression: A Fast Alternative to GZIP Compression (2026)  
   https://kinsta.com/blog/brotli-compression/

9. **Moncef Abboud** — Taking a Look at Compression Algorithms (2025)  
   https://cefboud.com/posts/compression/

10. **Command Line Fanatic** — A Completely Dissected GZIP File  
    https://commandlinefanatic.com/cgi-bin/showarticle.cgi?article=art053

11. **OneUptime** — How to Configure Compression for APIs (2026)  
    https://oneuptime.com/blog/post/2026-01-24-configure-api-compression/view

12. **CubePath** — Gzip/Brotli Compression in Apache and Nginx  
    https://cubepath.com/en/docs/performance-optimization/gzip-brotli-compression-in-apache-and-nginx

13. **NGINX Extras** — Enable Brotli Compression on NGINX  
    https://nginx-extras.getpagespeed.com/guides/brotli-compression/

14. **Beano** — Asset Compression with Brotli for Improved Website Performance (2019)  
    https://blog.beano.io/posts/asset-compression-with-brotli/

15. **IORiver** — GZIP vs Brotli Compression: Which Is Best for Web Performance (2025)  
    https://www.ioriver.io/blog/gzip-vs-brotli-compression-performance

---

## 📝 Примечания

- Все данные актуальны на апрель 2026 г.
- Рекомендации основаны на практике использования с веб-серверами nginx/Apache и CDN (Akamai, Cloudflare)
- Для специфических случаев (IoT, мобильные сети) могут потребоваться корректировки порогов
