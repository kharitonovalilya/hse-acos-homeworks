#!/bin/sh
# Each invocation below is a separate program, with no fork inside the server.
set -eu
program=${1:-railway_independent}
session=${2:-demo_$$}
left=${3:-5}
right=${4:-4}
case "$program" in railway_independent|railway_queue) ;; *) exit 1 ;; esac
mkdir -p results
log="$(pwd)/results/${program}_${session}_$$.log"
./"$program" --server "$session" --left "$left" --right "$right" \
    --arrival-max 0 --travel 8 --capacity 3 --gap 1 --max-group 3 \
    --delay-ms 20 --log "$log" &
server=$!
pids=""
cleanup() {
    kill -TERM "$server" $pids 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 4' INT TERM
tries=0
while ! test -s "$log"; do
    kill -0 "$server" 2>/dev/null || exit 1
    tries=$((tries + 1))
    test "$tries" -lt 500 || exit 1
    sleep 0.01
done
i=1
while test "$i" -le "$((left + right))"; do
    ./"$program" --train "$session" "$i" &
    pids="$pids $!"
    i=$((i + 1))
done
status=0
wait "$server" || status=$?
for pid in $pids; do wait "$pid" || status=1; done
trap - EXIT
exit "$status"
