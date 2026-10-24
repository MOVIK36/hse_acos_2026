#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
BIN="${1:-./sort_center}"
if [ ! -x "$BIN" ]; then
    make
fi

# Проверяем завершение и количество посылок на нескольких примерах.
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

run_test() {
    local name="$1"
    local expected="$2"
    shift 2
    local log="$TEST_DIR/$name.log"
    timeout 10 "$BIN" "$@" -D 0 -q -L "$log" > /dev/null
    grep -q 'ЗАВЕРШЕНО УСПЕШНО' "$log"
    grep -q 'Незавершённых посылок: 0' "$log"
    local arrived
    local finished
    arrived=$(grep -c '\[PARCEL_ARRIVAL' "$log" || true)
    finished=$(grep -Ec '\[(OUTPUT_DELIVERED|MANUAL_REJECT) *\]' "$log" || true)
    if [ "$arrived" -ne "$expected" ] || [ "$finished" -ne "$expected" ]; then
        echo "$name: ошибка в количестве посылок"
        exit 1
    fi
    echo "$name: OK"
}

run_test normal 25 -c data/normal.conf
run_test bottleneck 30 -c data/bottleneck.conf
run_test high_error 35 -c data/high_error.conf
run_test cli 15 -n 15 -d 4 -e 25
run_test small_output 12 -c data/small_buffers.conf -e 0
run_test small_manual 12 -c data/small_buffers.conf -e 100 -r 0
