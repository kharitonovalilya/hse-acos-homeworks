#!/bin/sh
set -e

make railway
mkdir -p results

./railway --left 3 --right 3 --capacity 1 --strategy fifo \
    --arrival-min 0 --arrival-max 4 --travel 3 --gap 1 \
    --seed 11 --log results/simple.log

./railway --left 5 --right 4 --capacity 3 --strategy priority \
    --arrival-min 0 --arrival-max 6 --travel 4 --gap 1 --max-group 3 \
    --seed 17 --log results/group.log

if ./railway --left 3 --right 3 --until 3 --seed 23 \
    --log results/partial.log; then
    exit 1
else
    status=$?
    test "$status" -eq 2
fi
