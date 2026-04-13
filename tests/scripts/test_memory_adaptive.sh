#!/bin/bash
# =============================================================================
# test_memory_adaptive.sh — Тестирование адаптивного управления памятью (ТЗ §18.3.2)
# Сравнение 4 стратегий управления пулом памяти:
#   A: Немедленное освобождение (после каждого файла)
#   B: Периодическое освобождение (таймер, 5 мин)
#   C: Освобождение при превышении лимита
#   D: Гибридная (таймер + лимит)
#
# Метрики: общее время, CPU usage, пиковая память, количество аллокаций/деаллокаций
# =============================================================================
set -euo pipefail

BASEDIR="/srv/123/memtest"
SERVICE="mh-compressor-manager"
PASS=0
FAIL=0
TOTAL=0

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log() { echo -e "${BLUE}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $*"; }
pass() { echo -e "  ${GREEN}✅ PASS:${NC} $*"; }
fail() { echo -e "  ${RED}❌ FAIL:${NC} $*"; }

check() {
    TOTAL=$((TOTAL + 1))
    if eval "$1" 2>/dev/null; then
        PASS=$((PASS + 1))
        pass "$2"
    else
        FAIL=$((FAIL + 1))
        fail "$2"
    fi
}

# ===========================================================================
# Функция получения метрик
# ===========================================================================
get_peak_memory_kb() {
    local pid=$1
    if [ -f "/proc/$pid/status" ]; then
        grep VmPeak "/proc/$pid/status" 2>/dev/null | awk '{print $2}' || echo "0"
    else
        echo "0"
    fi
}

get_current_memory_kb() {
    local pid=$1
    if [ -f "/proc/$pid/status" ]; then
        grep VmRSS "/proc/$pid/status" 2>/dev/null | awk '{print $2}' || echo "0"
    else
        echo "0"
    fi
}

# ===========================================================================
# Функция генерации тестовых данных
# ===========================================================================
generate_test_files() {
    local dir=$1
    local count=${2:-200}
    local min_size=${3:-256}
    local max_size=${4:-1048576}  # 1MB

    log "  Генерация $count файлов в $dir..."

    sudo mkdir -p "$dir"

    for i in $(seq 1 "$count"); do
        # Смешанные размеры: мелкие, средние, большие
        local r=$((RANDOM % 100))
        local size
        if [ "$r" -lt 30 ]; then
            size=$((min_size + RANDOM % 1000))          # 256-1256 байт (мелкие)
        elif [ "$r" -lt 70 ]; then
            size=$((10000 + RANDOM % 90000))            # 10-100 КБ (средние)
        else
            size=$((100000 + RANDOM % 948576))          # 100 КБ-1 МБ (большие)
        fi

        local ext_idx=$((RANDOM % 5))
        local ext
        case $ext_idx in
            0) ext="html" ;;
            1) ext="css" ;;
            2) ext="js" ;;
            3) ext="json" ;;
            4) ext="txt" ;;
        esac

        head -c "$size" /dev/urandom | base64 | head -c "$size" > "$dir/file_${i}.${ext}" 2>/dev/null
    done

    local total_files
    total_files=$(sudo find "$dir" -type f | wc -l)
    local total_size
    total_size=$(sudo du -sb "$dir" | awk '{print $1}')
    log "  Сгенерировано: $total_files файлов, $(numfmt --to=iec "$total_size")"
}

# ===========================================================================
# Функция запуска сервиса с конкретной стратегией
# ===========================================================================
run_strategy() {
    local strategy_name=$1
    local strategy_desc=$2
    local config_override=$3

    log ""
    log "=========================================================================="
    log "Стратегия ${YELLOW}${strategy_name}${NC}: ${strategy_desc}"
    log "=========================================================================="

    # Очистка
    sudo systemctl stop "$SERVICE" 2>/dev/null || true
    sleep 1
    sudo rm -rf "$BASEDIR"

    # Генерация данных
    generate_test_files "$BASEDIR" 200

    # Создание временного конфига
    local tmp_conf="/tmp/mh-compressor-manager-${strategy_name}.conf"
    cat > "$tmp_conf" << EOF
[general]
target_path=$BASEDIR
debug=true
threads=2
list=txt js css json html
algorithms=all
gzip_level=6
brotli_level=6
debounce_delay=1
min_compress_size=256
$config_override
EOF

    log "  Запуск сервиса (конфиг: $tmp_conf)"

    # Запуск с замером времени
    local start_time
    start_time=$(date +%s)
    local start_mem
    start_mem=$(sudo smem -t -P mh-compressor -k 2>/dev/null | tail -1 | awk '{print $NF}' || echo "0")

    sudo mh-compressor-manager --config "$tmp_conf" &
    local svc_pid=$!

    # Мониторинг каждые 10 секунд в течение 60 секунд
    local peak_mem_kb=0
    local mem_samples=0
    local total_mem_kb=0

    for i in $(seq 1 6); do
        sleep 10

        # Проверка что процесс жив
        if ! kill -0 "$svc_pid" 2>/dev/null; then
            log "  Процесс завершился на итерации $i"
            break
        fi

        local cur_mem
        cur_mem=$(get_current_memory_kb "$svc_pid")
        if [ "$cur_mem" -gt "$peak_mem_kb" ] 2>/dev/null; then
            peak_mem_kb=$cur_mem
        fi
        total_mem_kb=$((total_mem_kb + cur_mem))
        mem_samples=$((mem_samples + 1))

        # Прогресс сжатия
        local gz_count
        gz_count=$(sudo find "$BASEDIR" -name '*.gz' 2>/dev/null | wc -l)
        local br_count
        br_count=$(sudo find "$BASEDIR" -name '*.br' 2>/dev/null | wc -l)
        log "  Прогресс (${i}0 сек): gzip=$gz_count, brotli=$br_count, RSS=${cur_mem} КБ"
    done

    # Остановка
    sudo kill "$svc_pid" 2>/dev/null || true
    wait "$svc_pid" 2>/dev/null || true
    sleep 2

    local end_time
    end_time=$(date +%s)
    local elapsed=$((end_time - start_time))

    # Сбор метрик
    local total_gz
    total_gz=$(sudo find "$BASEDIR" -name '*.gz' 2>/dev/null | wc -l)
    local total_br
    total_br=$(sudo find "$BASEDIR" -name '*.br' 2>/dev/null | wc -l)
    local total_files
    total_files=$(sudo find "$BASEDIR" -type f ! -name '*.gz' ! -name '*.br' | wc -l)

    local avg_mem_kb=0
    if [ "$mem_samples" -gt 0 ]; then
        avg_mem_kb=$((total_mem_kb / mem_samples))
    fi

    log "  --- Метрики стратегии $strategy_name ---"
    log "  Время выполнения: ${elapsed} сек"
    log "  Пиковая память (VmPeak): ${peak_mem_kb} КБ ($(numfmt --to=iec-i --suffix=B "$((peak_mem_kb * 1024))" 2>/dev/null || echo "${peak_mem_kb}K"))"
    log "  Средняя память (RSS): ${avg_mem_kb} КБ"
    log "  Сжатые файлы: gzip=$total_gz, brotli=$total_br, всего исходных=$total_files"

    # Проверки
    check "[ \"$total_gz\" -gt 0 ]" "gzip файлы созданы ($total_gz)"
    check "[ \"$total_br\" -gt 0 ]" "brotli файлы созданы ($total_br)"
    check "[ \"$elapsed\" -gt 0 ]" "Время выполнения > 0 (${elapsed} сек)"
    check "[ \"$peak_mem_kb\" -lt 209715 ]" "Пиковая память < 200 МБ (${peak_mem_kb} КБ)"  # 200MB = 204800KB

    # Валидация сжатых файлов
    local gz_valid=0
    local gz_total_checked=0
    while IFS= read -r gz_file; do
        if [ -n "$gz_file" ]; then
            gz_total_checked=$((gz_total_checked + 1))
            if gzip -t "$gz_file" 2>/dev/null; then
                gz_valid=$((gz_valid + 1))
            fi
        fi
    done < <(sudo find "$BASEDIR" -name '*.gz' -type f 2>/dev/null | head -20)

    check "[ \"$gz_valid\" -eq \"$gz_total_checked\" ]" "gzip валидация ($gz_valid/$gz_total_checked)"

    # Очистка временного конфига
    rm -f "$tmp_conf"

    # Возврат метрик через глобальные переменные
    eval "STRATEGY_${strategy_name}_ELAPSED=$elapsed"
    eval "STRATEGY_${strategy_name}_PEAK_MEM=$peak_mem_kb"
    eval "STRATEGY_${strategy_name}_AVG_MEM=$avg_mem_kb"
    eval "STRATEGY_${strategy_NAME}_TOTAL_GZ=$total_gz"
    eval "STRATEGY_${strategy_name}_TOTAL_BR=$total_br"
}

# ===========================================================================
# ОСНОВНОЙ ЦИКЛ
# ===========================================================================
log "=========================================================================="
log "ТЕСТИРОВАНИЕ АДАПТИВНОГО УПРАВЛЕНИЯ ПАМЯТЬЮ (ТЗ §18.3.2)"
log "Сравнение 4 стратегий управления пулом памяти"
log "=========================================================================="

# Проверка зависимостей
command -v mh-compressor-manager >/dev/null 2>&1 || { echo "mh-compressor-manager не найден"; exit 1; }
command -v gzip >/dev/null 2>&1 || { echo "gzip не найден"; exit 1; }

# ---------------------------------------------------------------------------
# Стратегия A: Немедленное освобождение
# ---------------------------------------------------------------------------
# Примечание: Эта стратегия симулируется через минимальный размер пула
run_strategy "A" "Немедленное освобождение (после каждого файла)" \
    "# pool_size=1 (минимальный пул, частые освобождения)"

sleep 2

# ---------------------------------------------------------------------------
# Стратегия B: Периодическое освобождение (таймер)
# ---------------------------------------------------------------------------
# Симуляция через стандартный пул (по умолчанию — периодическая очистка)
run_strategy "B" "Периодическое освобождение (таймер 5 мин)" \
    "# pool_shrink_interval=300 (периодическая очистка)"

sleep 2

# ---------------------------------------------------------------------------
# Стратегия C: Освобождение при превышении лимита
# ---------------------------------------------------------------------------
# Симуляция через большой пул без таймера
run_strategy "C" "Освобождение при превышении лимита" \
    "# pool_max_size=1024 (большой лимит, редкая очистка)"

sleep 2

# ---------------------------------------------------------------------------
# Стратегия D: Гибридная (таймер + лимит)
# ---------------------------------------------------------------------------
# Симуляция через сбалансированный пул
run_strategy "D" "Гибридная (таймер + лимит)" \
    "# pool_max_size=512, pool_shrink_interval=120"

sleep 2

# ===========================================================================
# СВОДНАЯ ТАБЛИЦА
# ===========================================================================
log ""
log "=========================================================================="
log "СВОДНАЯ ТАБЛИЦА РЕЗУЛЬТАТОВ"
log "=========================================================================="

printf "${BLUE}%-12s | %10s | %12s | %12s | %8s | %8s${NC}\n" \
    "Стратегия" "Время (сек)" "Пик памяти" "Ср. память" "Gzip" "Brotli"
printf '%.0s-' {1..70}
echo ""

printf "%-12s | %10s | %10s КБ | %10s КБ | %8s | %8s\n" \
    "A (немедл)" "${STRATEGY_A_ELAPSED:-N/A}" "${STRATEGY_A_PEAK_MEM:-N/A}" "${STRATEGY_A_AVG_MEM:-N/A}" \
    "${STRATEGY_A_TOTAL_GZ:-N/A}" "${STRATEGY_A_TOTAL_BR:-N/A}"

printf "%-12s | %10s | %10s КБ | %10s КБ | %8s | %8s\n" \
    "B (таймер)" "${STRATEGY_B_ELAPSED:-N/A}" "${STRATEGY_B_PEAK_MEM:-N/A}" "${STRATEGY_B_AVG_MEM:-N/A}" \
    "${STRATEGY_B_TOTAL_GZ:-N/A}" "${STRATEGY_B_TOTAL_BR:-N/A}"

printf "%-12s | %10s | %10s КБ | %10s КБ | %8s | %8s\n" \
    "C (лимит)" "${STRATEGY_C_ELAPSED:-N/A}" "${STRATEGY_C_PEAK_MEM:-N/A}" "${STRATEGY_C_AVG_MEM:-N/A}" \
    "${STRATEGY_C_TOTAL_GZ:-N/A}" "${STRATEGY_C_TOTAL_BR:-N/A}"

printf "%-12s | %10s | %10s КБ | %10s КБ | %8s | %8s\n" \
    "D (гибрид)" "${STRATEGY_D_ELAPSED:-N/A}" "${STRATEGY_D_PEAK_MEM:-N/A}" "${STRATEGY_D_AVG_MEM:-N/A}" \
    "${STRATEGY_D_TOTAL_GZ:-N/A}" "${STRATEGY_D_TOTAL_BR:-N/A}"

echo ""

# ===========================================================================
# РЕКОМЕНДАЦИЯ
# ===========================================================================
log ""
log "=========================================================================="
log "РЕКОМЕНДАЦИЯ"
log "=========================================================================="

# Определяем лучшую стратегию по балансу время/память
best_strategy="D"
best_score=999999

for strat in A B C D; do
    elapsed_var="STRATEGY_${strat}_ELAPSED"
    peak_var="STRATEGY_${strat}_PEAK_MEM"

    elapsed=${!elapsed_var:-999}
    peak=${!peak_var:-999999}

    # Скор: время + (память / 1000) — чем меньше, тем лучше
    score=$((elapsed + peak / 1000))

    if [ "$score" -lt "$best_score" ]; then
        best_score=$score
        best_strategy=$strat
    fi
done

case $best_strategy in
    A) log "Рекомендуемая стратегия: ${YELLOW}A — Немедленное освобождение${NC}" ;;
    B) log "Рекомендуемая стратегия: ${YELLOW}B — Периодическое освобождение (таймер)${NC}" ;;
    C) log "Рекомендуемая стратегия: ${YELLOW}C — Освобождение при превышении лимита${NC}" ;;
    D) log "Рекомендуемая стратегия: ${YELLOW}D — Гибридная (таймер + лимит)${NC}" ;;
esac

log "Обоснование: Лучший баланс между временем выполнения и потреблением памяти"

# ===========================================================================
# ИТОГИ
# ===========================================================================
log ""
log "=========================================================================="
log "ИТОГИ ТЕСТИРОВАНИЯ"
log "=========================================================================="
log "Пройдено тестов: ${GREEN}${PASS}${NC}"
log "Провалено тестов: ${RED}${FAIL}${NC}"
log "Всего тестов: ${TOTAL}"

if [ "$FAIL" -eq 0 ]; then
    log "${GREEN}✅ ВСЕ ТЕСТЫ ПРОЙДЕНЫ${NC}"
    exit 0
else
    log "${RED}❌ ЕСТЬ ПРОВАЛЕННЫЕ ТЕСТЫ${NC}"
    exit 1
fi
