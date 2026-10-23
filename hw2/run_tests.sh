#!/bin/sh
set -eu

mkdir -p results
./tests > results/tests.log
./tests_cond > results/tests_cond.log

for program in railway railway_cond; do
    if ./$program --capacity 0 > results/invalid.log 2>&1; then
        echo "FAILED: invalid capacity"
        exit 1
    else
        code=$?
        test "$code" -eq 1
    fi

    if ./$program --left 1 --right 1 --arrival-max 0 --travel 100 --until 2 \
        --delay-ms 20 --log results/limit.log > results/limit_console.log; then
        echo "FAILED: time limit"
        exit 1
    else
        code=$?
        test "$code" -eq 2
    fi

    for signal in INT TERM; do
        ./$program --left 2 --right 2 --arrival-max 0 --travel 100 \
            --delay-ms 100 --log results/signal.log > results/signal_console.log &
        pid=$!
        sleep 0.1
        kill -"$signal" "$pid"
        if wait "$pid"; then
            echo "FAILED: interruption"
            exit 1
        else
            code=$?
            test "$code" -eq 4
        fi
    done
done

echo "All tests passed: 22 C checks for each version and 8 command-line checks."
