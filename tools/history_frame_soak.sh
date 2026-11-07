#!/bin/bash
# Opt-in real UI interframe diagnostic; only its own disposable DB/project.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
usage() {
    echo 'usage: tools/history_frame_soak.sh [--seconds N | --minutes N]'
    echo 'Default: about 30s of frame waits at 60fps; --minutes 60 opts into a long run.'
    echo 'Requires tools/services.sh up and an app built with frame_metrics.c.'
    echo 'SCRIBODO_SOAK_APP selects an executable snapshot (default build/scribodo).'
    echo 'Reports actual interframe/sample duration and sampled app-only RSS.'
    echo 'Fake backend/no LLM; excludes service RSS; not an input-latency SLA.'
}
seconds=30
if [[ ${1:-} == --help && $# == 1 ]]; then usage; exit 0; fi
if [[ $# == 2 && ( $1 == --seconds || $1 == --minutes ) && $2 =~ ^[1-9][0-9]{0,4}$ ]]; then
    seconds=$2
    [[ $1 != --minutes ]] || seconds=$((seconds * 60))
elif [[ $# != 0 ]]; then
    usage >&2; exit 2
fi
if (( seconds > 86400 )); then echo 'Duration must be at most 24 hours.' >&2; exit 2; fi
cd "$here"
app=${SCRIBODO_SOAK_APP:-build/scribodo}
if [[ ! -f $app || ! -x $app ]]; then echo "Build an executable app first: $app" >&2; exit 1; fi
[[ $app == /* ]] || app="$here/$app"
port=${SCRIBODO_DB_PORT-33061}
if [[ ! $port =~ ^[1-9][0-9]{0,4}$ ]] || (( port > 65535 )); then
    echo 'invalid SCRIBODO_DB_PORT (expected 1..65535)' >&2; exit 2
fi
name="scribodo_test_frames_$$_$(openssl rand -hex 8)"
temp=$(mktemp -d "${TMPDIR:-/tmp}/scribodo-frame-soak.XXXXXXXX")
created=0
cleanup() {
    rc=$?
    trap - EXIT
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
python3 tests/seed_chat_window.py "$temp/work/windowbook"
python3 - "$seconds" "$temp/soak.scribo" <<'PY'
from pathlib import Path
import math, sys
seconds = int(sys.argv[1])
text = Path('tests/ui/store/11_frame_soak.scribo').read_text()
prefix, rest = text.split('# SOAK_CYCLE_BEGIN\n')
cycle, suffix = rest.split('# SOAK_CYCLE_END\n')
# No script loops are needed. Keep roughly 10-second-or-shorter holds and
# repeat the real editing/navigation/cancellation path throughout the run.
repeats = max(1, math.ceil(seconds / 20))
hold = max(1, seconds * 60 // (2 * repeats))
Path(sys.argv[2]).write_text(prefix + cycle.replace('wait 600', f'wait {hold}') * repeats + suffix)
print(f'frame_soak: requested_wait_seconds={seconds} cycles={repeats} hold_frames={hold}', flush=True)
PY
echo "frame_soak: disposable_database=$name"
# Arm before create: creation can succeed even if a later grant fails.
created=1
tools/services.sh testdb create "$name"
start=$SECONDS
SCRIBODO_STORE=db SCRIBODO_DB_NAME="$name" SCRIBODO_DB_PORT="$port" SCRIBODO_LLM=fake \
    SCRIBODO_MODEL_SETTINGS="$temp/work/model-settings.json" SCRIBODO_PROFILE="$temp/work/profile.json" \
    SCRIBODO_SCRIPT="$temp/soak.scribo" SCRIBODO_SCRIPT_DIR="$temp/work" \
    SCRIBODO_SHOT_DIR="$temp" tools/watchdog.sh "$((seconds * 3 + 180))" \
    frame-soak python3 tools/frame_soak_run.py "$app"
echo "frame_soak: process_wall_seconds=$((SECONDS - start)) requested_wait_seconds=$seconds"
