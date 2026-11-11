#!/bin/bash
# Независимые программы запускает оболочка, координатор создаёт только посылки.
set -eu
cd "$(dirname "$0")/.."
BIN="${BIN:-./sort_center}"
SESSION="ind_$$"
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Счётчики участников совпадают с настройками из командной строки.
# Остальные параметры (включая -n, -e, -D, -L) можно передавать скрипту.
"$BIN" "$@" --mode independent --session "$SESSION" -l 2 -s 2 -o 1 -d 3 &
owner=$!
PIDS+=("$owner")
for role in scanner sorter conveyor service source; do
    case "$role" in
        scanner|source) count=2 ;;
        conveyor) count=3 ;;
        *) count=1 ;;
    esac
    for ((i=0; i<count; ++i)); do
        "$BIN" --worker "$SESSION" "$role" "$i" &
        PIDS+=("$!")
    done
done
result=0
wait "$owner" || result=$?
for pid in "${PIDS[@]:1}"; do wait "$pid" || result=1; done
PIDS=()
exit "$result"
