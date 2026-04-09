#!/bin/bash
# Слияние существующего /etc/mediahive/compressor-manager.conf с шаблоном из пакета:
# — для [general] подставляются строки из установленного файла по совпадению ключа;
# — все секции [folder_override:...] из установленного файла сохраняются;
# — из шаблона до MERGE_BOUNDARY подтягиваются новые ключи и комментарии;
# — пользовательские ключи, которых нет в шаблоне, дописываются перед folder_override.
#
# Использование: merge-compressor-conf.sh <шаблон.conf> <текущий_файл> <выходной_файл>

set -euo pipefail

if [[ ${BASH_VERSINFO[0]} -lt 4 ]]; then
    echo "merge-compressor-conf.sh: нужен bash 4+ (ассоциативные массивы)" >&2
    exit 1
fi

TEMPLATE=${1:?шаблон}
EXISTING=${2:?текущий файл}
OUT=${3:?выход}

strip_cr() {
    local s=$1
    while [[ "$s" == *$'\r' ]]; do
        s="${s%$'\r'}"
    done
    printf '%s' "$s"
}

BOUNDARY=$(grep -n '^# mh-compressor-manager: MERGE_BOUNDARY' "$TEMPLATE" | head -1 | cut -d: -f1) || true
if [[ -z "${BOUNDARY:-}" ]]; then
    BOUNDARY=$(grep -n 'ИНДИВИДУАЛЬНЫЕ НАСТРОЙКИ ДЛЯ ПАПОК' "$TEMPLATE" | head -1 | cut -d: -f1) || true
fi
if [[ -z "${BOUNDARY:-}" || ! "$BOUNDARY" =~ ^[0-9]+$ ]]; then
    echo "merge-compressor-conf.sh: не найдена граница MERGE_BOUNDARY в шаблоне" >&2
    exit 1
fi

declare -A user_kv
declare -A template_has

while IFS= read -r raw || [[ -n "$raw" ]]; do
    line=$(strip_cr "$raw")
    [[ "$line" =~ ^[[:space:]]*# ]] && continue
    [[ "$line" =~ ^[[:space:]]*$ ]] && continue
    [[ "$line" =~ ^([^=[:space:]]+)[[:space:]]*=(.*)$ ]] || continue
    user_kv["${BASH_REMATCH[1]}"]="$line"
done < <(awk '
    { gsub(/\r/, "") }
    /^\[general\]/ { g=1; next }
    g && /^\[/ { exit }
    g && /^[[:space:]]*#/ { next }
    g && /^[[:space:]]*$/ { next }
    g && /=/ { print }
' "$EXISTING")

while IFS= read -r raw || [[ -n "$raw" ]]; do
    line=$(strip_cr "$raw")
    [[ "$line" =~ ^[[:space:]]*# ]] && continue
    [[ "$line" =~ ^[[:space:]]*$ ]] && continue
    if [[ "$line" =~ ^[[:space:]]*([A-Za-z0-9_]+)[[:space:]]*= ]]; then
        template_has["${BASH_REMATCH[1]}"]=1
    fi
done < <(head -n "$((BOUNDARY - 1))" "$TEMPLATE")

emit_folder_banner=0
emit_folder_header() {
    if [[ $emit_folder_banner -eq 0 ]]; then
        printf '\n# --- Переопределения для папок (сохранено с предыдущей установки) ---\n'
        emit_folder_banner=1
    fi
}

{
    while IFS= read -r raw || [[ -n "$raw" ]]; do
        line=$(strip_cr "$raw")
        if [[ "$line" =~ ^[[:space:]]*# ]] || [[ "$line" =~ ^[[:space:]]*$ ]]; then
            printf '%s\n' "$line"
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*([A-Za-z0-9_]+)[[:space:]]*= ]]; then
            k="${BASH_REMATCH[1]}"
            if [[ -n "${user_kv[$k]+x}" ]]; then
                printf '%s\n' "${user_kv[$k]}"
                continue
            fi
        fi
        printf '%s\n' "$line"
    done < <(head -n "$((BOUNDARY - 1))" "$TEMPLATE")

    orphan_keys=()
    for k in "${!user_kv[@]}"; do
        [[ -z "${template_has[$k]+x}" ]] && orphan_keys+=("$k")
    done
    if [[ ${#orphan_keys[@]} -gt 0 ]]; then
        printf '\n# --- Сохранённые пользовательские параметры (нет в шаблоне текущей версии) ---\n'
        mapfile -t sorted < <(printf '%s\n' "${orphan_keys[@]}" | sort)
        for k in "${sorted[@]}"; do
            printf '%s\n' "${user_kv[$k]}"
        done
    fi

    folder_buf=""
    in_folder=0
    while IFS= read -r raw || [[ -n "$raw" ]]; do
        line=$(strip_cr "$raw")
        if [[ "$line" =~ ^\[folder_override: ]]; then
            if [[ $in_folder -eq 1 ]]; then
                emit_folder_header
                printf '%s\n\n' "$folder_buf"
            fi
            folder_buf="$line"
            in_folder=1
            continue
        fi
        if [[ $in_folder -eq 1 ]]; then
            if [[ "$line" =~ ^\[ ]] && [[ ! "$line" =~ ^\[folder_override: ]]; then
                emit_folder_header
                printf '%s\n\n' "$folder_buf"
                folder_buf=""
                in_folder=0
            else
                folder_buf+=$'\n'"$line"
            fi
        fi
    done < "$EXISTING"
    if [[ $in_folder -eq 1 && -n "$folder_buf" ]]; then
        emit_folder_header
        printf '%s\n\n' "$folder_buf"
    fi

    tail -n "+${BOUNDARY}" "$TEMPLATE"
} >"$OUT"
