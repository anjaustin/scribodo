#!/bin/bash
# Opt-in real UI interframe diagnostic; only its own disposable DB/project.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
usage() {
    echo 'usage: tools/history_frame_soak.sh [--manuscript] [--memory] [--real] [--cycle-seconds N] [--sample-seconds N] [--seconds N | --minutes N]'
    echo '--manuscript soaks the 100K-word fixture project (tests/ui/store/17_manuscript_soak.scribo) instead of the window book.'
    echo '--memory (with --manuscript) enables memory over the whole chat first (24_manuscript_soak_memory.scribo), so every send is ingested and indexed.'
    echo '--real uses the pinned local model for the sends (needs make llama and make model); the default is the fake model.'
    echo '--cycle-seconds N is the measured work of one cycle without its hold, used to size the run (default 2 fake, 90 real: MEMORY-019 phase 5).'
    echo '--sample-seconds N samples the store (tools/store_growth_sample.sh: bytes, rows, outbox, observer lag, points) every N s (default 60, 0 off).'
    echo 'Default: about 30s of frame waits at 60fps; --minutes 60 opts into a long run.'
    echo 'Requires tools/services.sh up and an app built with frame_metrics.c.'
    echo 'SCRIBODO_SOAK_APP selects an executable snapshot (default build/scribodo).'
    echo 'Reports actual interframe/sample duration and sampled app-only RSS.'
    echo 'Fake backend unless --real; excludes service RSS from the app figure; not an input-latency SLA.'
}
seconds=30; manuscript=0; memory=0; llm=fake; cycle=; sample=60
while (( $# )); do
    case "$1" in
        --help) usage; exit 0 ;;
        --manuscript) manuscript=1; shift ;;
        --memory) memory=1; shift ;;
        --real) llm=real; shift ;;
        --cycle-seconds) [[ $# -ge 2 && $2 =~ ^[1-9][0-9]{0,3}$ ]] || { usage >&2; exit 2; }; cycle=$2; shift 2 ;;
        --sample-seconds) [[ $# -ge 2 && $2 =~ ^[0-9]{1,4}$ ]] || { usage >&2; exit 2; }; sample=$2; shift 2 ;;
        --seconds|--minutes)
            [[ $# -ge 2 && $2 =~ ^[1-9][0-9]{0,4}$ ]] || { usage >&2; exit 2; }
            seconds=$2
            [[ $1 != --minutes ]] || seconds=$((seconds * 60))
            shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if (( memory && !manuscript )); then echo '--memory needs --manuscript' >&2; exit 2; fi
[[ -n $cycle ]] || cycle=$([[ $llm == real ]] && echo 90 || echo 2)
cd "$here"
tools/power_source.sh # a long run on battery hibernates mid-way (2026-11-13); exit 3 unless SCRIBODO_SOAK_ON_BATTERY=1
app=${SCRIBODO_SOAK_APP:-build/scribodo}
[[ -x $app ]] || { echo "Build an executable app first: $app" >&2; exit 1; }
[[ $app == /* ]] || app="$here/$app"
port=${SCRIBODO_DB_PORT-33061}
if ! [[ $port =~ ^[1-9][0-9]{0,4}$ ]] || (( port > 65535 )); then
    echo 'invalid SCRIBODO_DB_PORT (expected 1..65535)' >&2; exit 2
fi
name="scribodo_test_frames_$$_$(openssl rand -hex 8)"
temp=$(mktemp -d "${TMPDIR:-/tmp}/scribodo-frame-soak.XXXXXXXX")
created=0
sampler=
cleanup() {
    rc=$?
    trap - EXIT
    [[ -z $sampler ]] || { kill "$sampler" 2>/dev/null || true; wait "$sampler" 2>/dev/null || true; }
    if (( created )); then
        if ! tools/services.sh testdb drop "$name"; then
            echo "Cleanup failed; retry: tools/services.sh testdb drop $name" >&2
            [[ $rc != 0 ]] || rc=1
        else
            echo "frame_soak: cleaned_database=$name"
        fi
    fi
    # Only the exact directory returned by mktemp, never a supplied project.
    python3 - "$temp" <<'PY'
import shutil, sys
shutil.rmtree(sys.argv[1])
PY
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir "$temp/work"
if (( manuscript )); then
    python3 tests/seed_manuscript.py "$temp/work/manuscript" --words 100000 --per-category 4
    soak_source=tests/ui/store/17_manuscript_soak.scribo
    (( !memory )) || soak_source=tests/ui/store/24_manuscript_soak_memory.scribo
else
    python3 tests/seed_chat_window.py "$temp/work/windowbook"
    soak_source=tests/ui/store/11_frame_soak.scribo
fi
python3 - "$seconds" "$temp/soak.scribo" "$soak_source" "$cycle" <<'PY'
from pathlib import Path
import math, sys
seconds = int(sys.argv[1])
text = Path(sys.argv[3]).read_text()
prefix, rest = text.split('# SOAK_CYCLE_BEGIN\n')
cycle, suffix = rest.split('# SOAK_CYCLE_END\n')
# No script loops are needed. Keep roughly 10-second-or-shorter holds and
# repeat the real editing/navigation/cancellation path throughout the run.
# Measured cycle work: about 2 s for the manuscript cycle with the fake model
# (MEMORY-019 soak: 2,042 s / 120 cycles with 15 s holds), about 1 s for the
# window book; with the real model the send alone is the baseline's phase 5
# (--cycle-seconds, default 90).
cycle_work = int(sys.argv[4]) if 'manuscript' in sys.argv[3] else 1
hold_seconds = 15
repeats = max(1, math.ceil(seconds / (hold_seconds + cycle_work)))
hold = max(1, (seconds // repeats - cycle_work) * 60)
Path(sys.argv[2]).write_text(prefix + cycle.replace('wait 600', f'wait {hold}') * repeats + suffix)
print(f'frame_soak: requested_wait_seconds={seconds} cycles={repeats} hold_frames={hold}', flush=True)
PY
echo "frame_soak: disposable_database=$name"
# Arm before create: creation can succeed even if a later grant fails.
created=1
tools/services.sh testdb create "$name"
if (( sample )); then
    # The store, sampled while the app runs: one STORE_GROWTH line per sample, the first before the app opens.
    ( while :; do tools/store_growth_sample.sh "$name" soak || true; sleep "$sample"; done ) &
    sampler=$!
fi
start=$SECONDS
SCRIBODO_STORE=db SCRIBODO_DB_NAME="$name" SCRIBODO_DB_PORT="$port" SCRIBODO_SOAK_LLM="$llm" \
    SCRIBODO_MEMORY_EXPERIMENT="$memory" \
    SCRIBODO_MODEL_SETTINGS="$temp/work/model-settings.json" SCRIBODO_PROFILE="$temp/work/profile.json" \
    SCRIBODO_SCRIPT="$temp/soak.scribo" SCRIBODO_SCRIPT_DIR="$temp/work" \
    SCRIBODO_SHOT_DIR="$temp" caffeinate -i tools/watchdog.sh "$((seconds * 3 + 180 + memory * 600))" \
    frame-soak python3 tools/frame_soak_run.py "$app"
echo "frame_soak: process_wall_seconds=$((SECONDS - start)) requested_wait_seconds=$seconds model=$llm memory=$memory cycle_seconds=$cycle"
SCRIBODO_SOAK_ON_BATTERY=1 tools/power_source.sh # the source at the end, so a run that lost AC power says so
if [[ -n $sampler ]]; then
    kill "$sampler" 2>/dev/null || true; wait "$sampler" 2>/dev/null || true; sampler=
    tools/store_growth_sample.sh "$name" final || true
fi
