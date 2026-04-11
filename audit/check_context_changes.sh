#!/usr/bin/env bash
# check_context_changes.sh — Проверка изменений файлов с момента загрузки в контекст
#
# Использование:
#   bash check_context_changes.sh              # Проверить изменения
#   bash check_context_changes.sh --update     # Обновить метку времени (после загрузки в контекст)
#   bash check_context_changes.sh --reset      # Сбросить метку времени

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TIMESTAMP_FILE="${SCRIPT_DIR}/.context/last_loaded.txt"
CONFIG_FILE="${PROJECT_DIR}/.settings"

# Файлы для отслеживания
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

# Получение метки времени загрузки в контекст
get_context_timestamp() {
    if [[ -f "$TIMESTAMP_FILE" ]]; then
        source "$TIMESTAMP_FILE"
        echo "${CONTEXT_TIMESTAMP:-0}"
    else
        echo "0"
    fi
}

# Обновление метки времени
update_timestamp() {
    local ts
    ts=$(date +%s)
    echo "CONTEXT_TIMESTAMP=${ts}" > "$TIMESTAMP_FILE"
    echo "✅ Метка времени обновлена: $(date -d "@${ts}" '+%Y-%m-%d %H:%M:%S')"
}

# Сброс метки времени
reset_timestamp() {
    echo "CONTEXT_TIMESTAMP=0" > "$TIMESTAMP_FILE"
    echo "🔄 Метка времени сброшена"
}

# Проверка изменений
check_changes() {
    local context_ts
    context_ts=$(get_context_timestamp)

    if [[ "$context_ts" == "0" ]]; then
        echo "⚠️  Метка времени не установлена. Загрузите документацию в контекст и выполните:"
        echo "    bash audit/check_context_changes.sh --update"
        echo ""
        echo "📋 Список файлов для проверки:"
        for f in "${DOC_FILES[@]}"; do
            if [[ -f "${PROJECT_DIR}/${f}" ]]; then
                local mod_ts
                mod_ts=$(stat -c %Y "${PROJECT_DIR}/${f}" 2>/dev/null || echo "недоступен")
                echo "   ${f} (изменён: $(date -d "@${mod_ts}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "недоступен"))"
            else
                echo "   ${f} (НЕ НАЙДЕН)"
            fi
        done
        return 0
    fi

    local context_date
    context_date=$(date -d "@${context_ts}" '+%Y-%m-%d %H:%M:%S')
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
                local ago
                ago=$((mod_ts - context_ts))
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
            changed_files+=(".settings (изменён: ${mod_date})")
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
        echo "   3. Обнови метку времени: bash audit/check_context_changes.sh --update"
        return 1
    else
        echo "✅ Документация не изменялась с момента загрузки в контекст"
        return 0
    fi
}

# Основная логика
case "${1:-}" in
    --update)
        update_timestamp
        ;;
    --reset)
        reset_timestamp
        ;;
    *)
        check_changes
        ;;
esac
