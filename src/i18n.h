#pragma once
#include <cstdlib>
#include <string>
#include <string_view>
#include <format>

/**
 * @brief Локализация приложения (i18n).
 * Один файл для всего проекта. Язык определяется по env LANG при первом вызове.
 * По умолчанию — английский. При LANG=ru_* — русский.
 *
 * Простые строки:
 *   Logger::info(_("File compressed", "Файл сжат"));
 *
 * Строки с форматированием:
 *   Logger::info(tr_fmt("File: {} bytes", "Файл: {} байт", size));
 */
namespace i18n {

inline bool is_russian() {
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

/**
 * @brief Перевод + форматирование строки (runtime).
 * Использует std::vformat вместо std::format для поддержки runtime строк.
 */
template<typename... Args>
std::string tr_fmt(const char* en, const char* ru, Args&&... args) {
    return std::vformat(is_russian() ? ru : en,
                        std::make_format_args(std::forward<Args>(args)...));
}

} // namespace i18n

// Короткие макросы
#define _(en, ru) i18n::tr(en, ru)
#define _fmt(en, ru, ...) i18n::tr_fmt(en, ru, __VA_ARGS__)
