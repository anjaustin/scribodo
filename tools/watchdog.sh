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
launched=$(date '+%Y-%m-%d %H:%M:%S')
# Hold the machine awake while the command runs. The limit below is wall
# time: an idle sleep inside it fires the watchdog at the next wake and
# reports a sleeping process as a hang (measured 2026-11-08: test_llm
# launched 11:35:32, the Mac slept at 11:35:39, the sample fired at 11:53:09
# on wake with the main thread in a plain usleep). caffeinate exits with the
# command, including when the watchdog kills it.
if command -v caffeinate >/dev/null 2>&1; then
    caffeinate -i -w "$pid" 2>/dev/null &
fi
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
        # Record the machine's sleep/wake events since launch beside the sample,
        # so a reader can tell a hang from a nap without consulting pmset.
        if command -v pmset >/dev/null 2>&1; then
            naps=$(pmset -g log 2>/dev/null | grep -E 'Entering Sleep|Wake from|DarkWake' \
                   | awk -v s="$launched" 'substr($0,1,19) >= s')
            {
                echo "== Power events since launch ($launched) =="
                if [ -n "$naps" ]; then echo "$naps"; else echo "(none)"; fi
            } >> "$out"
            if [ -n "$naps" ]; then
                echo "WATCHDOG: the machine slept or woke $(echo "$naps" | wc -l | tr -d ' ') time(s) during the run; see $out" >&2
            fi
        fi
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
