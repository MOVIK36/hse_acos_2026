#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
BIN="${1:-.}"
TEST_DIR=$(mktemp -d)
IP=127.0.0.1
PORT="${TEST_PORT:-$((20000 + $$ % 30000))}"

cleanup() {
    local pid
    for pid in $(jobs -pr); do kill -TERM "$pid" 2>/dev/null || true; done
    wait || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for_line() {
    local pattern="$1" file="$2"
    for ((i=0; i<300; ++i)); do
        if grep -q "$pattern" "$file" 2>/dev/null; then return 0; fi
        sleep 0.01
    done
    echo "Не найдено '$pattern' в $file" >&2
    return 1
}

start_server() {
    local name="$1"
    shift
    log="$TEST_DIR/$name.log"
    "$BIN/sort_center" --server "$IP" "$PORT" "$@" -q -L "$log" > "$TEST_DIR/server" &
    server=$!
    wait_for_line '\[SIM_START' "$log"
}

start_sources() {
    sources=()
    for ((line=1; line<=$1; ++line)); do
        "$BIN/sort_source" "$IP" "$PORT" "$line" > "$TEST_DIR/source_$line" &
        sources+=("$!")
    done
}

check_parcels() {
    local expected="$1"
    sed -n '/\[PARCEL_ARRIVAL/{s/.*#\([0-9]*\).*/\1/p;}' "$log" | sort > "$TEST_DIR/arrived"
    sed -n '/\[OUTPUT_DELIVERED\|\[MANUAL_REJECT/{s/.*#\([0-9]*\).*/\1/p;}' "$log" | sort > "$TEST_DIR/finished"
    sed -n '/\[PARCEL_END/{s/.*#\([0-9]*\).*/\1/p;}' "$log" | sort > "$TEST_DIR/processes"
    cmp "$TEST_DIR/arrived" "$TEST_DIR/finished"
    cmp "$TEST_DIR/arrived" "$TEST_DIR/processes"
    [ "$(wc -l < "$TEST_DIR/arrived")" -eq "$expected" ]
    [ "$(sort -u "$TEST_DIR/arrived" | wc -l)" -eq "$expected" ]
    grep -q 'ЗАВЕРШЕНО УСПЕШНО' "$log"
    grep -q 'Незавершённых посылок: 0' "$log"
    local session
    session=$(sed -n 's/.*сессия: //p' "$log")
    [ -n "$session" ]
    [ ! -e "/dev/shm/sc_$session" ]
    ! compgen -G "/dev/shm/sem.sc_${session}_s*" > /dev/null
}

run_test() {
    local name="$1" expected="$2" lines="$3"
    shift 3
    start_server "$name" "$@" -l "$lines" -D 0
    "$BIN/sort_observer" "$IP" "$PORT" > "$TEST_DIR/observer" &
    local observer=$!
    wait_for_line '\[SIM_START' "$TEST_DIR/observer"
    start_sources "$lines"
    wait "$server"
    local pid
    for pid in "${sources[@]}"; do wait "$pid"; done
    wait "$observer"
    cmp "$log" "$TEST_DIR/observer"
    check_parcels "$expected"
    sed -n 's/.*сервер принял посылку #\([0-9]*\)$/\1/p' "$TEST_DIR"/source_* | sort > "$TEST_DIR/accepted"
    cmp "$TEST_DIR/arrived" "$TEST_DIR/accepted"
    rm "$TEST_DIR"/source_*
    echo "tcp/$name: OK"
}

run_test normal 25 2 -c data/normal.conf
run_test bottleneck 30 3 -c data/bottleneck.conf
run_test high_error 35 2 -c data/high_error.conf
run_test cli 15 2 -n 15 -d 4 -e 25 --mode named
run_test small_output 12 2 -c data/small_buffers.conf -e 0 --output-interval 60
grep -q '\[OUTPUT_STALL' "$log"
run_test small_manual 12 2 -c data/small_buffers.conf -e 100 -r 0 --manual-interval 60
grep -q '\[MANUAL_STALL' "$log"
grep -q 'Повторных сканирований: 0' "$log"
run_test retry_limit 6 2 -n 6 -e 100 -r 2
grep -q 'Повторных сканирований: 12' "$log"
run_test reuse 300 2 -n 300 -e 35

start_server empty -n 0
wait "$server"
check_parcels 0
echo "tcp/empty: OK"

start_server reconnect -c data/small_buffers.conf -l 2 -n 20 -e 0 -D 10
"$BIN/sort_observer" "$IP" "$PORT" > "$TEST_DIR/first" &
first=$!
wait_for_line '\[SIM_START' "$TEST_DIR/first"
start_sources 2
wait_for_line '\[OUTPUT_DELIVERED' "$TEST_DIR/first"
"$BIN/sort_observer" "$IP" "$PORT" > "$TEST_DIR/second" &
second=$!
wait_for_line '\[OUTPUT_DELIVERED' "$TEST_DIR/second"
kill -TERM "$first"
wait "$first" || true
"$BIN/sort_observer" "$IP" "$PORT" > "$TEST_DIR/reconnected" &
reconnected=$!
wait_for_line '\[OUTPUT_DELIVERED' "$TEST_DIR/reconnected"
wait "$server"
for pid in "${sources[@]}"; do wait "$pid"; done
wait "$second"
wait "$reconnected"
cmp "$log" "$TEST_DIR/second"
cmp "$log" "$TEST_DIR/reconnected"
check_parcels 20
echo "tcp/reconnect: OK"

for sig in INT TERM; do
    start_server "stop-$sig" -l 1 --unlimited -D 5
    "$BIN/sort_observer" "$IP" "$PORT" > "$TEST_DIR/observer" &
    observer=$!
    wait_for_line '\[SIM_START' "$TEST_DIR/observer"
    start_sources 1
    wait_for_line '\[PARCEL_ARRIVAL' "$TEST_DIR/observer"
    kill -"$sig" "$server"
    wait "$server"
    wait "${sources[0]}"
    wait "$observer"
    cmp "$log" "$TEST_DIR/observer"
    grep -q 'ПРЕРВАНО ПО ЗАПРОСУ' "$log"
    echo "tcp/stop-$sig: OK"
done

start_server source_disconnect -l 2 --unlimited
"$BIN/sort_observer" "$IP" "$PORT" > "$TEST_DIR/observer" &
observer=$!
wait_for_line '\[SIM_START' "$TEST_DIR/observer"
start_sources 1
wait_for_line '\[SOURCE_CONNECT' "$TEST_DIR/observer"
kill -TERM "${sources[0]}"
wait "${sources[0]}" || true
if wait "$server"; then
    echo "Сервер должен завершиться с ошибкой при отключении источника" >&2
    exit 1
fi
wait "$observer"
cmp "$log" "$TEST_DIR/observer"
grep -q 'Статус: ОШИБКА' "$log"
echo "tcp/source_disconnect: OK"
