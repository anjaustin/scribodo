#!/bin/bash
# C03 checkpoint verification, all model and shared-yardstick stages serial.
# No DB/UI/author project is opened; --live uses the existing synthetic state fixture.
set -eu
scratch=${1:?Usage: run-redteam-gates.sh SCRATCH NEW_OUTPUT}
out=${2:?New output directory required}
root=$(git rev-parse --show-toplevel)
cd "$root"
mkdir "$out"
export HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 HF_HUB_DISABLE_TELEMETRY=1
export TMPDIR=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/
export SCRIBODO_MODEL="$HOME/.cache/huggingface/hub/models--LiquidAI--LFM3-1.7B-GGUF/snapshots/cd0c74c575b5c416f3011b5b4c298077f26ca8c2/LFM3-1.7B-BF16.gguf"
gate() {
    name=$1
    shift
    set +e
    "$@" > "$out/$name.log" 2>&1
    code=$?
    set -e
    printf '%s\t%s\n' "$name" "$code" >> "$out/exits.tsv"
    test "$code" -eq 0 || exit "$code"
}
idle() {
    set +e
    pgrep -x llama-server > "$out/pre-$1-processes.txt"
    code=$?
    set -e
    test "$code" -eq 1 || { echo "Existing server present; leave untouched"; exit 1; }
}
gate oracle python3 tests/test_c03_acceptance.py
idle baseline
gate live-baseline make -j1 llmtest LLAMA_BIN="$root/build/llama/bin/llama-server"
idle candidate
gate live-candidate make -j1 llmtest LLAMA_BIN="$scratch/build/bin/llama-server"
idle roles-baseline
gate roles-baseline tools/watchdog.sh 900 c03-roles-baseline "$scratch/reference-env/bin/python" \
    journal/composition001/c03/role_smoke.py --server "$root/build/llama/bin/llama-server" \
    --label baseline --out "$out/roles-baseline"
idle roles-candidate
gate roles-candidate tools/watchdog.sh 900 c03-roles-candidate "$scratch/reference-env/bin/python" \
    journal/composition001/c03/role_smoke.py --server "$scratch/build/bin/llama-server" \
    --label candidate --out "$out/roles-candidate"
idle repository
gate test make -j1 test
gate analyze make -j1 analyze
gate sanitize make -j1 sanitize
gate tsan make -j1 tsan
gate whitespace git diff --check
