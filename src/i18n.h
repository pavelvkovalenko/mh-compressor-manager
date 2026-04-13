#pragma once
#include <cstdlib>
#include <cstring>
#include <clocale>
#include <string_view>

#ifdef HAVE_GETTEXT
#include <libintl.h>
#endif

/**
 * @brief Локализация приложения (i18n) — единый файл для всего проекта.
 *
 * Архитектура: GNU gettext (подход coreutils/systemd).
 * Исходный код содержит только английские строки.
 * Переводы — в отдельных .po/.mo файлах.
 * При отсутствии .mo — программа работает на английском без ошибок.
 *
 * Использование:
 *   // Простая строка:
 *   Logger::info(_("File compressed"));
 *
 *   // Строка с аргументами (printf-style):
 *   Logger::info(_("Compressed %s: %zu bytes"), path.c_str(), size);
 *
 * Компиляция с gettext:
 *   -DHAVE_GETTEXT - подключить libintl
 * Без gettext (fallback):
 *   -DHAVE_GETTEXT не определён → dgettext(d,s) → (s) → возвращает оригинал
 */

// Fallback: если gettext недоступен, макрос возвращает строку как есть
#ifndef HAVE_GETTEXT
#define dgettext(domain, msgid) (msgid)
#endif

/**
 * @brief Перевод строки через dgettext().
 * @param msgid Оригинальная (английская) строка
 * @return Переведённая строка или оригинал, если перевод не найден
 */
#define _(msgid) dgettext("mh-compressor-manager", msgid)

/**
 * @brief Пометка строки для извлечения без перевода (статические массивы).
 */
#define N_(msgid) (msgid)

namespace i18n {

/**
 * @brief Инициализация локализации.
 * Вызывается один раз в main(), до первого использования _().
 *
 * @param locale_dir Путь к каталогу с .mo файлами (обычно /usr/share/locale)
 */
inline void init(const char* locale_dir = "/usr/share/locale") {
    // Инициализация locale из переменных окружения (LANG, LC_ALL, LC_MESSAGES)
    std::setlocale(LC_ALL, "");

#ifdef HAVE_GETTEXT
    // Привязка домена переводов к каталогу
    bindtextdomain("mh-compressor-manager", locale_dir);
    // Установка кодировки домена
    bind_textdomain_codeset("mh-compressor-manager", "UTF-8");
    // Установка домена по умолчанию
    textdomain("mh-compressor-manager");
#else
    // Fallback: gettext недоступен, используем оригинальные строки
    (void)locale_dir;
#endif
}

} // namespace i18n
