#!/bin/sh
set -eu
mkdir -p results
./tests > results/tests.log
./tests_named > results/tests_named.log

server=""
train=""
cleanup() {
    if test -n "$train"; then kill -TERM "$train" 2>/dev/null || true; fi
    if test -n "$server"; then kill -TERM "$server" 2>/dev/null || true; fi
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 1' INT TERM

wait_for_event() {
    tries=0
    while ! grep -q "event=$1" "$log" 2>/dev/null; do
        tries=$((tries + 1))
        test "$tries" -lt 500 || { echo "FAILED: expected $1"; exit 1; }
        sleep 0.01
    done
}

check_resources() {
    for object in "/dev/shm/acos27_$session" "/dev/mqueue/acos27_$session" \
                  /dev/shm/sem.acos27_"${session}"_s*; do
        test ! -e "$object" || { echo "FAILED: IPC object remains: $object"; exit 1; }
    done
}

for program in railway railway_named railway_independent railway_queue; do
    for signal in INT TERM; do
        log="$(pwd)/results/${program}_${signal}.log"
        console="results/${program}_${signal}_console.log"
        rm -f "$log" "$console"
        case "$program" in
            railway_independent|railway_queue)
                session="signal_$$_${program}_${signal}"
                set -- --server "$session"
                ;;
            *) set -- ;;
        esac
        ./"$program" "$@" --left 1 --right 0 --arrival-max 0 --travel 100 \
            --delay-ms 100 --log "$log" >> "$console" 2>&1 &
        server=$!
        case "$program" in
            railway_independent|railway_queue)
                wait_for_event CONFIG
                ./"$program" --train "$session" 1 >> "$console" 2>&1 &
                train=$!
                ;;
            *) session="fork_$server" ;;
        esac
        wait_for_event ENTER
        kill -"$signal" "$server"
        code=0
        wait "$server" || code=$?
        server=""
        test "$code" -eq 4 || { echo "FAILED: $program $signal returned $code"; exit 1; }
        if test -n "$train"; then
            code=0
            wait "$train" || code=$?
            train=""
            test "$code" -eq 4 || { echo "FAILED: train returned $code"; exit 1; }
        fi
        grep -q 'event=SUMMARY' "$log"
        check_resources
        echo "PASSED: $program SIG$signal and IPC cleanup"
    done
done

for program in railway_independent railway_queue; do
    session="log_$$_${program}"
    log="$(pwd)/results/${program}_log_error.log"
    console="results/${program}_log_error_console.log"
    rm -f "$log" "$console"
    ./"$program" --server "$session" --left 1 --right 0 --log "$log" \
        >> "$console" 2>&1 &
    server=$!
    wait_for_event CONFIG
    # The server still has the file open, but a new train cannot open its path.
    mv "$log" "$log.saved"
    code=0
    ./"$program" --train "$session" 1 >> "$console" 2>&1 || code=$?
    test "$code" -eq 1 || { echo "FAILED: train log error returned $code"; exit 1; }
    code=0
    wait "$server" || code=$?
    server=""
    test "$code" -eq 1 || { echo "FAILED: server log error returned $code"; exit 1; }
    mv "$log.saved" "$log"
    if grep -q 'event=REGISTER' "$log"; then
        echo "FAILED: train registered without its log"
        exit 1
    fi
    grep -q 'event=SUMMARY' "$log"
    check_resources
    echo "PASSED: $program log error stops the server and removes IPC"
done

trap - EXIT
echo "All tests passed: 44 C checks and 10 shell checks."
