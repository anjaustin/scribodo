#!/bin/bash
# Restart and resume at manuscript scale (GAP-003): the memory soak cycle runs on a disposable database and project
# until at least --cycles cycles have landed, the app is killed with SIGKILL mid-run, the store is sampled, and the
# app is started again on the same database and folder under tests/ui/store/25_manuscript_resume.scribo. Reports the
# resume's FRAME_METRICS (open, recall, send), STORE_GROWTH before the kill, after it and once memory has settled
# again, and whether the message count is what the kill left (no loss, no duplicate). Fake model unless --real.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
usage() {
    echo 'usage: tools/manuscript_restart.sh [--cycles N] [--real] [--settle-seconds N] [--out DIR]'
    echo '--cycles N          soak cycles to let land before the kill (default 3; each cycle is one send).'
    echo '--real              the pinned model for the sends (default fake).'
    echo '--settle-seconds N  how long to wait for memory to settle after the restart (default 600).'
    echo '--out DIR           where run logs go (default build/gap003/restart-<stamp>).'
    echo 'Requires tools/services.sh up and a built app.'
}
cycles=3; llm=fake; settle=600; out=
while (( $# )); do
    case "$1" in
        --help) usage; exit 0 ;;
        --cycles) [[ $# -ge 2 && $2 =~ ^[1-9][0-9]?$ ]] || { usage >&2; exit 2; }; cycles=$2; shift 2 ;;
        --real) llm=real; shift ;;
        --settle-seconds) [[ $# -ge 2 && $2 =~ ^[1-9][0-9]{0,4}$ ]] || { usage >&2; exit 2; }; settle=$2; shift 2 ;;
        --out) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; out=$2; shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
cd "$here"
app=${SCRIBODO_SOAK_APP:-build/scribodo}
[[ -x $app ]] || { echo "Build an executable app first: $app" >&2; exit 1; }
[[ $app == /* ]] || app="$here/$app"
port=${SCRIBODO_DB_PORT-33061}
[[ -n $out ]] || out="build/gap003/restart-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
name="scribodo_test_restart_$$_$(openssl rand -hex 8)"
temp=$(mktemp -d "${TMPDIR:-/tmp}/scribodo-restart.XXXXXXXX")
created=0; app_pid=
cleanup() {
    rc=$?
    trap - EXIT
    [[ -z $app_pid ]] || kill -9 "$app_pid" 2>/dev/null || true
    if (( created )); then
        tools/services.sh testdb drop "$name" || { echo "Cleanup failed; retry: tools/services.sh testdb drop $name" >&2; [[ $rc != 0 ]] || rc=1; }
    fi
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
python3 tests/seed_manuscript.py "$temp/work/manuscript" --words 100000 --per-category 4
# The soak script with enough cycles to outlast the kill: short holds, the kill comes from outside.
python3 - "$temp/soak.scribo" tests/ui/store/24_manuscript_soak_memory.scribo "$((cycles + 20))" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[2]).read_text()
prefix, rest = text.split('# SOAK_CYCLE_BEGIN\n')
cycle, suffix = rest.split('# SOAK_CYCLE_END\n')
Path(sys.argv[1]).write_text(prefix + cycle.replace('wait 600', 'wait 120') * int(sys.argv[3]) + suffix)
PY
sample() { tools/store_growth_sample.sh "$name" "$1" | tee -a "$out/store.log"; }
messages_of() { sed -n 's/.* messages=\([0-9]*\) .*/\1/p' <<<"$1"; }
settled() { [[ $1 =~ outbox_pending=0\ outbox_leased=0\ outbox_failed=0 ]] && [[ $1 =~ scope_cursor=([0-9-]+)\ scope_pending=[0-9-]+\ max_seq=([0-9-]+) ]] && [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]; }
echo "manuscript_restart: disposable_database=$name cycles=$cycles model=$llm out=$out tree=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" | tee "$out/restart.log"
created=1
tools/services.sh testdb create "$name"
env_common=(SCRIBODO_STORE=db SCRIBODO_DB_NAME="$name" SCRIBODO_DB_PORT="$port" SCRIBODO_SOAK_LLM="$llm" SCRIBODO_MEMORY_EXPERIMENT=1
            SCRIBODO_MODEL_SETTINGS="$temp/work/model-settings.json" SCRIBODO_PROFILE="$temp/work/profile.json"
            SCRIBODO_SCRIPT_DIR="$temp/work" SCRIBODO_SHOT_DIR="$temp")
# Phase 1: the soak, killed from outside once the cycles have landed.
env "${env_common[@]}" SCRIBODO_SCRIPT="$temp/soak.scribo" caffeinate -i python3 tools/frame_soak_run.py "$app" > "$out/soak.log" 2>&1 &
runner=$!
target=$((1116 + 2 * cycles))
per_cycle=30; [[ $llm != real ]] || per_cycle=120  # the fake cycle is seconds, the real one a reply (MEMORY-019)
deadline=$((SECONDS + 600 + cycles * per_cycle))
line=
while (( SECONDS < deadline )); do
    sleep 5
    kill -0 "$runner" 2>/dev/null || { echo "manuscript_restart: the soak ended before the kill (see $out/soak.log)" | tee -a "$out/restart.log"; exit 1; }
    line=$(sample soak)
    m=$(messages_of "$line")
    [[ -z $m ]] || (( m < target )) || break
done
app_pid=$(sed -n 's/.*owned_app_pid=\([0-9]*\).*/\1/p' "$out/soak.log" | head -1)
[[ -n $app_pid ]] || { echo "manuscript_restart: no app pid in $out/soak.log" | tee -a "$out/restart.log"; exit 1; }
before=$(sample before_kill)
kill -9 "$app_pid"
killed_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
wait "$runner" || true
app_pid=
echo "manuscript_restart: killed app at $killed_at after $(messages_of "$before") messages (SIGKILL, mid-cycle)" | tee -a "$out/restart.log"
sleep 2
after=$(sample after_kill)
# Phase 2: the same database and folder, the resume script.
start=$SECONDS
env "${env_common[@]}" SCRIBODO_SCRIPT=tests/ui/store/25_manuscript_resume.scribo caffeinate -i tools/watchdog.sh 1800 \
    manuscript-resume python3 tools/frame_soak_run.py "$app" > "$out/resume.log" 2>&1 || echo "manuscript_restart: resume script exited non-zero (see $out/resume.log)" | tee -a "$out/restart.log"
echo "manuscript_restart: resume_wall_seconds=$((SECONDS - start))" | tee -a "$out/restart.log"
grep -h "FRAME_METRICS\|passed, \|FRAME_SOAK_RSS" "$out/resume.log" | sed 's/ (includes.*//' | cut -c1-160 | tee -a "$out/restart.log"
# Phase 3: does memory settle again on its own? The resume script sent one message; the kill may have left pieces.
final=$(sample after_resume)
deadline=$((SECONDS + settle))
while ! settled "$final" && (( SECONDS < deadline )); do sleep 10; final=$(sample settling); done
if settled "$final"; then echo "manuscript_restart: memory settled after the restart" | tee -a "$out/restart.log"
else echo "manuscript_restart: memory NOT settled within ${settle}s: $final" | tee -a "$out/restart.log"; fi
# Conservation: the resume added exactly the resume script's two messages (its question and the reply).
mb=$(messages_of "$after"); mf=$(messages_of "$final")
echo "manuscript_restart: messages before_kill=$(messages_of "$before") after_kill=$mb after_resume=$mf (expected after_resume = after_kill + 2)" | tee -a "$out/restart.log"
[[ $mf == $((mb + 2)) ]] && settled "$final" && grep -q "0 failed" "$out/resume.log"
