#!/bin/bash
# Headless real SQL/Qdrant/worker pipeline; fake embeddings/tokens, no LLM/UI.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
usage() {
    echo 'usage: tools/memory_pipeline_soak.sh [--seconds N | --minutes N] [--toggle]'
    echo 'Default 30s; maximum 24h. --toggle adds worker pause/reselection stress.'
    echo 'Needs tools/services.sh up. Builds existing core/LLM objects, no UI.'
    echo 'Warmup and one collection-loss recovery add bounded time to the workload.'
    echo 'Fake embedding/tokenizer durability/resource workload, not model quality or an SLA.'
}
seconds=30
toggle=0
duration_seen=0
while (( $# )); do
    case "$1" in
        --help) [[ $# == 1 && $duration_seen == 0 && $toggle == 0 ]] || { usage >&2; exit 2; }; usage; exit 0 ;;
        --toggle) [[ $toggle == 0 ]] || { usage >&2; exit 2; }; toggle=1; shift ;;
        --seconds|--minutes)
            [[ $# -ge 2 && $duration_seen == 0 && $2 =~ ^[1-9][0-9]{0,4}$ ]] || { usage >&2; exit 2; }
            seconds=$2
            [[ $1 != --minutes ]] || seconds=$((seconds * 60))
            duration_seen=1; shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if (( seconds > 86400 )); then echo 'Duration must be at most 24 hours.' >&2; exit 2; fi
check_port() {
    if [[ ! $2 =~ ^[1-9][0-9]{0,4}$ ]] || (( $2 > 65535 )); then
        echo "invalid $1 (expected 1..65535)" >&2; exit 2
    fi
}
check_port SCRIBODO_DB_PORT "${SCRIBODO_DB_PORT-33061}"
check_port SCRIBODO_QDRANT_PORT "${SCRIBODO_QDRANT_PORT-6333}"
cd "$here"
mkdir -p build/memory-pipeline-soak
# An ephemeral make overlay uses the real project's object/library definitions;
# it neither modifies Makefile nor links main, UI, raylib, or a model process.
make --no-print-directory -f Makefile -f - memory-pipeline-soak-build <<'MAKE'
.PHONY: memory-pipeline-soak-build
memory-pipeline-soak-build: $(CORE_OBJS) $(LLM_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) tools/memory_pipeline_soak.c $(CORE_OBJS) $(LLM_OBJS) -o build/memory-pipeline-soak/memory_pipeline_soak $(MARIADB_LIBS) -lm -lpthread
MAKE
name="scribodo_test_memory_soak_$$_$(openssl rand -hex 8)"
temp=$(mktemp -d "${TMPDIR:-/tmp}/scribodo-memory-soak.XXXXXXXX")
created=0
cleanup() {
    rc=$?
    trap - EXIT
    if (( created )); then
        if ! tools/services.sh testdb drop "$name"; then
            echo "Cleanup failed; retry: tools/services.sh testdb drop $name" >&2
            [[ $rc != 0 ]] || rc=1
        else
            echo "memory_pipeline_soak: cleaned_database=$name"
        fi
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
printf '%s\n' "$name" > "$temp/owned-database"
echo "memory_pipeline_soak: disposable_database=$name requested_seconds=$seconds toggle=$toggle"
created=1
tools/services.sh testdb create "$name"
SCRIBODO_LLM=fake SCRIBODO_DB_NAME="$name" \
    tools/watchdog.sh "$((seconds * 2 + 900))" memory-pipeline-soak \
    build/memory-pipeline-soak/memory_pipeline_soak "$name" "$temp" "$seconds" "$toggle"
