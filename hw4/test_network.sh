#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

work=$(mktemp -d)
port=${RAILWAY_TEST_PORT:-$((20000 + RANDOM % 25000))}
cleanup() {
    for pid in $(jobs -pr); do kill "$pid" 2>/dev/null || true; done
    wait || true
    rm -r "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

wait_log() {
    local i
    for ((i=0; i<250; i++)); do
        if grep -q "$2" "$work/$1"; then return; fi
        sleep 0.02
    done
    echo "Missing '$2' in $1" >&2
    cat "$work/$1" "$work/server" >&2
    return 1
}

start_server() {
    ./railway_server 127.0.0.1 "$port" --delay-ms 20 --until 500 \
        --log "$work/events" "$@" >"$work/server" 2>&1 &
    server=$!
    wait_log server 'Сервер слушает'
}

observer() {
    ./railway_observer 127.0.0.1 "$port" >"$work/$1" 2>&1 &
    pid=$!
    wait_log "$1" '^SNAPSHOT'
}

train() {
    ./railway_train 127.0.0.1 "$port" "$2" >"$work/$1" 2>&1 &
    pid=$!
}

expect_status() {
    local status=0
    wait "$1" || status=$?
    if [[ "$status" != "$2" ]]; then
        echo "Process $1 exited with $status, expected $2" >&2
        return 1
    fi
}

check_events() {
    awk -v total="$1" -v peak="$2" -v group="$3" -v travel="$4" '
        /^time=/ {
            minute = substr($1, 6) + 0
            event = substr($2, 7)
            train = substr($3, 7) + 0
            direction = substr($4, 5)
            occupied = substr($5, 11) + 0
            if (event == "DIRECTION") {
                if (active) exit 1
                track = direction
                group_size = 0
            }
            if (event == "PERMIT") permit[train] = 1
            if (event == "REVOKE") delete permit[train]
            if (event == "ENTER") {
                if (!permit[train] || train in entered || direction != track ||
                    (entries && minute - last < 2) || ++group_size > group) exit 1
                delete permit[train]
                entered[train] = minute
                last = minute
                entries++
                active++
            }
            if (event == "EXIT") {
                if (!(train in entered) || train in exited || minute - entered[train] < travel) exit 1
                exited[train] = 1
                exits++
                active--
            }
            if (active != occupied || active > peak) exit 1
            if (active > maximum) maximum = active
        }
        END { if (entries != total || exits != total || maximum != peak) exit 1 }
    ' "$work/events"
}

start_server --left 2 --right 2 --capacity 1 --max-group 1 --strategy fifo
observer simple; watch=$pid
train simple1 1; first=$pid
train simple2 2; second=$pid
train simple3 3; third=$pid
train simple4 4; fourth=$pid
for child in "$server" "$watch" "$first" "$second" "$third" "$fourth"; do expect_status "$child" 0; done
check_events 4 1 1 5
grep -q 'SUMMARY reason=completed finished=4/4' "$work/simple"
echo 'OK: simple, FIFO'

start_server --left 2 --right 1 --arrival-max 0 --travel 60 --strategy fifo
observer watch1; watch1=$pid
observer watch2; watch2=$pid
kill -TERM "$watch1"
expect_status "$watch1" 4
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf TRA >&3
sleep 0.02
printf 'IN 1\n' >&3
line=
while IFS= read -r -t 5 line <&3; do
    if [[ "$line" == GO ]]; then break; fi
done
[[ "$line" == GO ]]
exec 3>&-
wait_log watch2 'event=REVOKE'
observer watch3; watch3=$pid
wait_log watch3 'state=WAITING connected=0'
train first_run 1; first=$pid
wait_log first_run 'event=ENTER'
kill -TERM "$first"
expect_status "$first" 4
train second_run 2; second=$pid
wait_log second_run 'event=ENTER'
train first_again 1; first=$pid
train third_run 3; third=$pid
for child in "$server" "$watch2" "$watch3" "$first" "$second" "$third"; do expect_status "$child" 0; done
check_events 3 2 3 60
grep -q 'SUMMARY reason=completed finished=3/3' "$work/watch3"
echo 'OK: group, two observers, reconnect, fragmented command'

for reason in interrupted time_limit; do
    start_server --left 1 --right 0 --arrival-min 300 --arrival-max 300 --until 125
    observer "$reason"; watch=$pid
    train valid 1; first=$pid
    train invalid 2; invalid=$pid
    expect_status "$invalid" 1
    wait_log invalid ERROR
    wait_log valid REGISTERED
    if [[ "$reason" == interrupted ]]; then
        kill -TERM "$server"
        expect_status "$server" 4
    else
        expect_status "$server" 2
    fi
    expect_status "$watch" 0
    expect_status "$first" 0
    grep -q "SUMMARY reason=$reason" "$work/$reason"
    grep -q 'Сервер завершил работу' "$work/valid"
    echo "OK: $reason"
done
