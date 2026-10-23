#!/bin/sh
set -eu

mkdir -p results
for program in railway railway_cond; do
    ./$program --left 3 --right 3 --arrival-min 0 --arrival-max 0 \
        --travel 3 --capacity 1 --gap 1 --max-group 1 --strategy fifo \
        --delay-ms 20 --log results/"$program"_simple.log
    ./$program --left 5 --right 4 --arrival-min 0 --arrival-max 0 \
        --travel 8 --capacity 3 --gap 1 --max-group 3 --strategy priority \
        --delay-ms 20 --log results/"$program"_group.log
done
