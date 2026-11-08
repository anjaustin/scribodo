#!/bin/bash
# Manuscript-scale UI baseline (MEMORY-015 S4): the real app, fake model, a
# disposable database and a disposable project built from the S2 fixture.
# Reports one FRAME_METRICS line per phase (import, chat open, paging, document
# edit, send) and the app's sampled RSS. Not an SLA; frame times include waits.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
usage() {
    echo 'usage: tools/manuscript_baseline.sh [--words N] [--per-category K] [--real] [--manuscript DIR]'
    echo '--manuscript DIR uses the chapter .md files of a real manuscript (tools/manuscript_import.py) as the documents.'
    echo '--script FILE drives a different UI script (default tests/ui/store/16_manuscript_baseline.scribo).'
    echo '--keep leaves the disposable database and the work directory in place for diagnosis (prints both).'
    echo '--real uses the pinned local model (slower; needs make llama and make model); default is the fake model.'
    echo 'Default: the 100K-word fixture with 4 items per category.'
    echo 'Requires tools/services.sh up and a built app (build/scribodo).'
}
words=100000; per=4; llm=fake; script=tests/ui/store/16_manuscript_baseline.scribo; keep=0
while (( $# )); do
    case "$1" in
        --real) llm=real; shift ;;
        --manuscript) manuscript=$2; shift 2 ;;
        --script) script=$2; shift 2 ;;
        --keep) keep=1; shift ;;
        --words) words=$2; shift 2 ;;
        --per-category) per=$2; shift 2 ;;
        --help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
cd "$here"
app=${SCRIBODO_SOAK_APP:-build/scribodo}
[[ -x $app ]] || { echo "Build an executable app first: $app" >&2; exit 1; }
[[ $app == /* ]] || app="$here/$app"
port=${SCRIBODO_DB_PORT-33061}
name="scribodo_test_manuscript_$$_$(openssl rand -hex 8)"
temp=$(mktemp -d "${TMPDIR:-/tmp}/scribodo-manuscript.XXXXXXXX")
created=0
cleanup() {
    rc=$?
    trap - EXIT
    if (( keep )); then
        echo "manuscript_baseline: kept database=$name work=$temp (drop with: tools/services.sh testdb drop $name)"
        exit "$rc"
    fi
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
python3 tests/seed_manuscript.py "$temp/work/manuscript" --words "$words" --per-category "$per" ${manuscript:+--manuscript "$manuscript"}
echo "manuscript_baseline: disposable_database=$name"
created=1
tools/services.sh testdb create "$name"
start=$SECONDS
SCRIBODO_STORE=db SCRIBODO_DB_NAME="$name" SCRIBODO_DB_PORT="$port" SCRIBODO_SOAK_LLM="$llm" SCRIBODO_MEMORY_EXPERIMENT=1 \
    SCRIBODO_MODEL_SETTINGS="$temp/work/model-settings.json" SCRIBODO_PROFILE="$temp/work/profile.json" \
    SCRIBODO_SCRIPT="$script" SCRIBODO_SCRIPT_DIR="$temp/work" \
    SCRIBODO_SHOT_DIR="$temp" caffeinate -i tools/watchdog.sh 3000 manuscript-baseline python3 tools/frame_soak_run.py "$app"
echo "manuscript_baseline: process_wall_seconds=$((SECONDS - start)) words=$words per_category=$per model=$llm script=$script manuscript=${manuscript:-fixture}"
