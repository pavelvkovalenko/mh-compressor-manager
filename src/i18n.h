#pragma once
#include <cstdlib>
#include <string_view>

/**
 * @brief Локализация приложения (i18n).
 * Один файл для всего проекта. Язык определяется по env LANG при первом вызове.
 * По умолчанию — английский. При LANG=ru_* — русский.
 *
 * Использование:
 *   Logger::info(_("File compressed", "Файл сжат"));
 *   Logger::error(_("Failed to open: %s", "Не удалось открыть: %s"), path.c_str());
 */
namespace i18n {

inline bool is_russian() {
    // Определяется один раз, кэшируется
    static const bool ru = []() {
        const char* lang = std::getenv("LANG");
        if (!lang) lang = std::getenv("LC_ALL");
        if (!lang) lang = std::getenv("LC_MESSAGES");
        if (!lang) return false;
        std::string_view sv(lang);
        return sv.starts_with("ru") || sv.starts_with("RU");
    }();
    return ru;
}

inline const char* tr(const char* en, const char* ru) {
    return is_russian() ? ru : en;
}

} // namespace i18n

// Короткий макрос для перевода
#define _(en, ru) i18n::tr(en, ru)
