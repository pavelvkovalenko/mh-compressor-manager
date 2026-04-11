#!/usr/bin/env bash
# check_context_changes.sh — Проверка изменений файлов с момента загрузки в контекст
#
# Метка времени ХРАНИТСЯ В КОНТЕКСТЕ БЕСЕДЫ (в памяти сессии), а не в файле.
# Это позволяет запускать параллельные ветки обсуждения без конфликтов.
#
# Использование:
#   bash audit/check_context_changes.sh <timestamp>     — Проверить изменения с указанной метки
#   bash audit/check_context_changes.sh --list-files    — Вывести список отслеживаемых файлов и их даты
#
# Аргумент <timestamp> — это Unix timestamp (секунды с epoch), сохранённый в контексте беседы
# после загрузки документации. Получить текущий timestamp: date +%s
#
# Пример сохранения метки в контекст (выполняет AI-ассистент):
#   Текущий timestamp: 1712850000 (2026-04-11 15:00:00)
#   Запомни: context_doc_timestamp=1712850000

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG_FILE="${PROJECT_DIR}/.settings"

# Файлы для отслеживания (относительно корня проекта)
DOC_FILES=(
    "docs/specification/TECHNICAL_SPECIFICATION.md"
    "docs/development/QWEN.md"
    "docs/development/RULES.md"
    "docs/development/DEPLOY.md"
    "CONTRIBUTING.md"
    "README.md"
    "audit/CHANGELOG.md"
    "audit/ROUND_HISTORY.md"
    "audit/AUDIT_CYCLE.md"
    "tests/TEST_SCRIPTS.md"
)

# Вывести список отслеживаемых файлов с датами изменений
list_files() {
    echo "📋 Отслеживаемые файлы документации:"
    echo ""
    for f in "${DOC_FILES[@]}"; do
        local filepath="${PROJECT_DIR}/${f}"
        if [[ -f "$filepath" ]]; then
            local mod_ts
            mod_ts=$(stat -c %Y "$filepath" 2>/dev/null || echo "0")
            local mod_date
            mod_date=$(date -d "@${mod_ts}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "недоступно")
            echo "   📄 ${f}"
            echo "      Изменён: ${mod_date}"
        else
            echo "   ❌ ${f} (НЕ НАЙДЕН)"
        fi
    done

    # Файл настроек
    if [[ -f "$CONFIG_FILE" ]]; then
        local mod_ts
        mod_ts=$(stat -c %Y "$CONFIG_FILE" 2>/dev/null || echo "0")
        local mod_date
        mod_date=$(date -d "@${mod_ts}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "недоступно")
        echo ""
        echo "   ⚙️  .settings"
        echo "      Изменён: ${mod_date}"
    fi
    echo ""
}

# Проверить изменения с указанной метки времени
check_changes() {
    local context_ts="$1"
    local now_ts
    now_ts=$(date +%s)
    local now_date
    now_date=$(date -d "@${now_ts}" '+%Y-%m-%d %H:%M:%S')
    local context_date
    context_date=$(date -d "@${context_ts}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "некорректная метка")

    echo "📅 Текущее время: ${now_date}"
    echo "📅 Документация загружена в контекст: ${context_date}"
    echo ""

    local changed_count=0
    local changed_files=()

    for f in "${DOC_FILES[@]}"; do
        local filepath="${PROJECT_DIR}/${f}"
        if [[ -f "$filepath" ]]; then
            local mod_ts
            mod_ts=$(stat -c %Y "$filepath" 2>/dev/null || echo "0")
            if [[ "$mod_ts" -gt "$context_ts" ]]; then
                local mod_date
                mod_date=$(date -d "@${mod_ts}" '+%Y-%m-%d %H:%M:%S')
                local ago=$((mod_ts - context_ts))
                local ago_str=""
                if [[ $ago -lt 60 ]]; then
                    ago_str="${ago} сек назад"
                elif [[ $ago -lt 3600 ]]; then
                    ago_str="$((ago / 60)) мин назад"
                elif [[ $ago -lt 86400 ]]; then
                    ago_str="$((ago / 3600)) ч назад"
                else
                    ago_str="$((ago / 86400)) дн назад"
                fi
                changed_files+=("${f} (изменён: ${mod_date}, ${ago_str})")
                ((changed_count++))
            fi
        fi
    done

    # Проверка файла настроек
    if [[ -f "$CONFIG_FILE" ]]; then
        local mod_ts
        mod_ts=$(stat -c %Y "$CONFIG_FILE" 2>/dev/null || echo "0")
        if [[ "$mod_ts" -gt "$context_ts" ]]; then
            local mod_date
            mod_date=$(date -d "@${mod_ts}" '+%Y-%m-%d %H:%M:%S')
            local ago=$((mod_ts - context_ts))
            local ago_str=""
            if [[ $ago -lt 60 ]]; then
                ago_str="${ago} сек назад"
            elif [[ $ago -lt 3600 ]]; then
                ago_str="$((ago / 60)) мин назад"
            elif [[ $ago -lt 86400 ]]; then
                ago_str="$((ago / 3600)) ч назад"
            else
                ago_str="$((ago / 86400)) дн назад"
            fi
            changed_files+=(".settings (изменён: ${mod_date}, ${ago_str})")
            ((changed_count++))
        fi
    fi

    if [[ $changed_count -gt 0 ]]; then
        echo "⚠️  ОБНАРУЖЕНЫ ИЗМЕНЕНИЯ В ДОКУМЕНТАЦИИ (${changed_count} файлов):"
        echo ""
        for cf in "${changed_files[@]}"; do
            echo "   🔴 ${cf}"
        done
        echo ""
        echo "📌 Действия:"
        echo "   1. Прочитай изменённые файлы: cat <файл>"
        echo "   2. Загрузи изменения в контекст беседы"
        echo "   3. Обнови метку времени в контексте (не в файле!)"
        echo ""
        echo "💡 Текущий timestamp для сохранения в контекст: $(date +%s)"
        return 1
    else
        echo "✅ Документация не изменялась с момента загрузки в контекст"
        return 0
    fi
}

# Основная логика
case "${1:-}" in
    --list-files)
        list_files
        ;;
    --help|-h)
        echo "Использование: bash audit/check_context_changes.sh <timestamp>"
        echo ""
        echo "  <timestamp>    Unix timestamp (секунды) — метка загрузки в контекст"
        echo "  --list-files   Вывести список отслеживаемых файлов с датами"
        echo "  --help         Показать эту справку"
        echo ""
        echo "Метка времени хранится в КОНТЕКСТЕ БЕСЕДЫ (в памяти сессии), а не в файле."
        echo "Это позволяет запускать параллельные ветки обсуждения без конфликтов."
        ;;
    "")
        echo "❌ Ошибка: не указана метка времени"
        echo ""
        echo "Использование: bash audit/check_context_changes.sh <timestamp>"
        echo ""
        echo "Метка времени должна быть передана как аргумент и храниться в контексте беседы."
        echo "Получить текущий timestamp: date +%s"
        echo ""
        echo "Пример:"
        echo "  bash audit/check_context_changes.sh 1712850000"
        exit 1
        ;;
    *)
        check_changes "$1"
        ;;
esac
