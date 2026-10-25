#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
BIN="${1:-./sort_center}"
if [ ! -x "$BIN" ]; then make; fi
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

run_test() {
    local name="$1" expected="$2"
    shift 2
    local log="$TEST_DIR/$name.log"
    timeout -k 1 10 "$BIN" "$@" -D 0 -q -L "$log" > /dev/null
    grep -q 'ЗАВЕРШЕНО УСПЕШНО' "$log"
    grep -q 'Незавершённых посылок: 0' "$log"
    local arrived finished threads
    arrived=$(grep -c '\[PARCEL_ARRIVAL' "$log" || true)
    finished=$(grep -Ec '\[(OUTPUT_DELIVERED|MANUAL_REJECT) *\]' "$log" || true)
    threads=$(grep -c '\[PARCEL_END' "$log" || true)
    if [ "$arrived" -ne "$expected" ] || [ "$finished" -ne "$expected" ] || [ "$threads" -ne "$expected" ]; then
        echo "$name: ошибка в количестве посылок или потоков"
        exit 1
    fi
    # Каждому поступившему ID соответствует ровно одно завершение обработки.
    sed -n '/\[PARCEL_ARRIVAL/{s/.*#\([0-9]*\).*/\1/p;}' "$log" | sort > "$TEST_DIR/arrived"
    sed -n '/\[OUTPUT_DELIVERED\|\[MANUAL_REJECT/{s/.*#\([0-9]*\).*/\1/p;}' "$log" | sort > "$TEST_DIR/finished"
    cmp "$TEST_DIR/arrived" "$TEST_DIR/finished"
    echo "$name: OK"
}

run_test normal 25 -c data/normal.conf
run_test bottleneck 30 -c data/bottleneck.conf
run_test high_error 35 -c data/high_error.conf
run_test cli 15 -n 15 -d 4 -e 25
run_test small_output 12 -c data/small_buffers.conf -e 0
run_test small_manual 12 -c data/small_buffers.conf -e 100 -r 0
grep -q 'Повторных сканирований: 0' "$TEST_DIR/small_manual.log"
run_test retry_limit 6 -n 6 -e 100 -r 2
grep -q 'Повторных сканирований: 12' "$TEST_DIR/retry_limit.log"
run_test empty 0 -n 0

# Остановка при заполненном выходе/ручной зоне: все ожидающие потоки должны выйти.
for sig in INT TERM; do
    log="$TEST_DIR/stop-$sig.log"
    timeout -k 1 5 bash -c '
        "$1" -c data/small_buffers.conf --unlimited -e "$4" -r 0 -D 0 -q \
            --output-interval 1000000 --manual-interval 1000000 -L "$2" > /dev/null &
        sim_pid=$!
        sleep 0.2
        kill -"$3" "$sim_pid"
        wait "$sim_pid"
    ' _ "$BIN" "$log" "$sig" "$([ "$sig" = INT ] && echo 0 || echo 100)"
    grep -q 'ПРЕРВАНО ПО ЗАПРОСУ' "$log"
    grep -q '\[SIM_INTERRUPT' "$log"
    echo "stop-$sig: OK"
done
