#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
BIN="${1:-./sort_center}"
if [ ! -x "$BIN" ]; then make; fi
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

# Независимые роли запускаются скриптом из отдельных команд оболочки.
run_model() {
    if [ "$mode" = independent ]; then
        BIN="$BIN" bash scripts/run_independent.sh "$@"
    else
        "$BIN" --mode "$mode" "$@"
    fi
}

check_cleanup() {
    local session
    session=$(sed -n 's/.*сессия: //p' "$1" | head -1)
    [ -n "$session" ]
    if compgen -G "/dev/shm/sc_${session}" > /dev/null ||
       compgen -G "/dev/shm/sem.sc_${session}_s*" > /dev/null; then
        echo "$mode: IPC-объекты не удалены"
        exit 1
    fi
}

run_test() {
    local name="$1" expected="$2"
    shift 2
    local log="$TEST_DIR/$mode-$name.log"
    run_model "$@" -D 0 -q -L "$log" > "$TEST_DIR/stdout"
    grep -q 'ЗАВЕРШЕНО УСПЕШНО' "$log"
    grep -q 'Незавершённых посылок: 0' "$log"
    local arrived finished processes started
    arrived=$(grep -c '\[PARCEL_ARRIVAL' "$log" || true)
    finished=$(grep -Ec '\[(OUTPUT_DELIVERED|MANUAL_REJECT) *\]' "$log" || true)
    processes=$(grep -c '\[PARCEL_END' "$log" || true)
    started=$(grep -c '\[PARCEL_START' "$log" || true)
    if [ "$arrived" -ne "$expected" ] || [ "$finished" -ne "$expected" ] ||
       [ "$processes" -ne "$expected" ] || [ "$started" -ne "$expected" ]; then
        echo "$mode/$name: ошибка в количестве посылок или процессов"
        exit 1
    fi
    # Каждому поступившему ID соответствует ровно одно завершение обработки.
    sed -n '/\[PARCEL_ARRIVAL/{s/.*#\([0-9]*\).*/\1/p;}' "$log" | sort > "$TEST_DIR/arrived"
    sed -n '/\[OUTPUT_DELIVERED\|\[MANUAL_REJECT/{s/.*#\([0-9]*\).*/\1/p;}' "$log" | sort > "$TEST_DIR/finished"
    sed -n '/\[PARCEL_END/{s/.*#\([0-9]*\).*/\1/p;}' "$log" | sort > "$TEST_DIR/processes"
    cmp "$TEST_DIR/arrived" "$TEST_DIR/finished"
    cmp "$TEST_DIR/arrived" "$TEST_DIR/processes"
    [ "$(sort -u "$TEST_DIR/arrived" | wc -l)" -eq "$expected" ]
    check_cleanup "$TEST_DIR/stdout"
    echo "$mode/$name: OK"
}

# Общий timeout задан Makefile: зависшая проверка не оставит make test ждать бесконечно.
for mode in unnamed named independent; do
    run_test normal 25 -c data/normal.conf
    run_test bottleneck 30 -c data/bottleneck.conf
    run_test high_error 35 -c data/high_error.conf
    run_test cli 15 -n 15 -d 4 -e 25
    run_test small_output 12 -c data/small_buffers.conf -e 0
    run_test small_manual 12 -c data/small_buffers.conf -e 100 -r 0
    grep -q 'Повторных сканирований: 0' "$TEST_DIR/$mode-small_manual.log"
    run_test retry_limit 6 -n 6 -e 100 -r 2
    grep -q 'Повторных сканирований: 12' "$TEST_DIR/$mode-retry_limit.log"
    run_test empty 0 -n 0
    # Проверка повторного использования 128 ячеек общей памяти.
    run_test reuse 300 -n 300 -e 35

    for sig in INT TERM; do
        log="$TEST_DIR/$mode-stop-$sig.log"
        if [ "$mode" = independent ]; then
            # Прерываем координатор напрямую. Участники увидят общий флаг остановки.
            session="stop_$$_$sig"
            "$BIN" --mode independent --session "$session" -l 1 -s 1 -o 1 -d 1 \
                --unlimited -D 0 -q -L "$log" > "$TEST_DIR/stdout" &
            owner=$!
            pids=()
            for role in scanner sorter conveyor service source; do
                "$BIN" --worker "$session" "$role" 0 &
                pids+=("$!")
            done
            sleep 0.2
            kill -"$sig" "$owner"
            wait "$owner"
            for pid in "${pids[@]}"; do wait "$pid"; done
        else
            "$BIN" --mode "$mode" -c data/small_buffers.conf --unlimited \
                -e "$([ "$sig" = INT ] && echo 0 || echo 100)" -r 0 -D 0 -q \
                --output-interval 1000000 --manual-interval 1000000 -L "$log" > "$TEST_DIR/stdout" &
            owner=$!
            sleep 0.2
            kill -"$sig" "$owner"
            wait "$owner"
        fi
        grep -q 'ПРЕРВАНО ПО ЗАПРОСУ' "$log"
        grep -q '\[SIM_INTERRUPT' "$log"
        check_cleanup "$TEST_DIR/stdout"
        echo "$mode/stop-$sig: OK"
    done
done
