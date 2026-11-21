#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

port=${1:-41027}
mkdir -p results
make all
pids=()
cleanup() {
    if (("${#pids[@]}")); then
        kill "${pids[@]}" 2>/dev/null || true
        wait 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

run_case() {
    local name=$1
    shift
    ./railway_server 127.0.0.1 "$port" --left 3 --right 3 --arrival-min 5 \
        --arrival-max 5 --delay-ms 100 --until 100 --log "results/$name.log" "$@" \
        > "results/${name}_server.log" 2>&1 &
    local server=$!
    pids+=("$server")
    for ((i=0; i<100; i++)); do
        if grep -q 'Сервер слушает' "results/${name}_server.log"; then break; fi
        kill -0 "$server"
        sleep 0.02
    done
    grep -q 'Сервер слушает' "results/${name}_server.log"
    ./railway_observer 127.0.0.1 "$port" > "results/${name}_observer.log" &
    pids+=("$!")
    for id in {1..6}; do
        ./railway_train 127.0.0.1 "$port" "$id" > "results/${name}_train$id.log" &
        pids+=("$!")
    done
    for pid in "${pids[@]}"; do wait "$pid"; done
    pids=()
    tail -n 1 "results/$name.log"
}

run_case simple --capacity 1 --max-group 1 --strategy fifo
run_case group --capacity 2 --max-group 3 --strategy priority
