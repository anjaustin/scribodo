#!/bin/bash
# watchdog.sh SECONDS NAME COMMAND [ARGS...]
#
# Runs COMMAND with a time limit. If it is still running after SECONDS, the
# watchdog captures a stack sample of the whole process (all threads) into
# build/hangs/NAME-<time>.txt, prints the main thread's innermost frames, then
# kills it and exits 124. Otherwise it exits with COMMAND's status.
#
# Use it for anything that could hang: tests, the app under scripts, servers.
set -u
limit=$1; name=$2; shift 2
mkdir -p build/hangs

"$@" &
pid=$!
start=$SECONDS
while kill -0 "$pid" 2>/dev/null; do
    if (( SECONDS - start >= limit )); then
        stamp=$(date +%H%M%S)
        out="build/hangs/${name}-${stamp}.txt"
        echo "WATCHDOG: '$name' still running after ${limit}s (pid $pid); sampling to $out" >&2
        # Sample the process and any children it spawned (e.g. the app under test).
        for p in "$pid" $(pgrep -P "$pid" 2>/dev/null); do
            sample "$p" 2 -file "$out.$p" >/dev/null 2>&1
            if [ -s "$out.$p" ]; then
                cat "$out.$p" >> "$out"
                rm -f "$out.$p"
            fi
        done
        if [ -s "$out" ]; then
            echo "WATCHDOG: main thread, innermost frames:" >&2
            # Print the deepest frames of the busiest main-thread path.
            awk '/com\.apple\.main-thread|Main Thread/{f=1; next} f && /^ *[0-9]+ Thread_/{exit} f && /[+!:|] /{print}' "$out" \
                | sed -E 's/^[ +!:|]*[0-9]+ //; s/ +\[0x[0-9a-f]+\]//' | head -40 | tail -8 | sed 's/^/    /' >&2
        fi
        pkill -TERM -P "$pid" 2>/dev/null
        kill -TERM "$pid" 2>/dev/null
        sleep 1
        pkill -KILL -P "$pid" 2>/dev/null
        kill -KILL "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        exit 124
    fi
    sleep 0.5
done
wait "$pid"
