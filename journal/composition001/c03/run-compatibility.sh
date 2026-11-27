#!/bin/bash
# Serial, offline, model-only compatibility stages. Never starts the product app or store.
set -eu
root=$(git rev-parse --show-toplevel)
cd "$root"
scratch=${1:?Usage: run-compatibility.sh SCRATCH NEW_OUTPUT}
python="$scratch/reference-env/bin/python"
out=${2:?A new isolated output directory is required}
test -x "$python"
mkdir "$out" # refuse reruns, including before opening/truncating any log
logs="$out/driver-logs"
mkdir "$logs"
export HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 HF_HUB_DISABLE_TELEMETRY=1
export HF_MODULES_CACHE="$scratch/hf-modules"
export TOKENIZERS_PARALLELISM=false
export PYTHONPATH="$root/third_party/llama.cpp/gguf-py"
export LFM3_FA=off LFM3_GGUF_FILE=LFM3-1.7B-BF16.gguf
probe=journal/composition001/c03/compatibility_probe.py
cp "$probe" "$out/probe-source.py"
run() {
    name=$1
    seconds=$2
    shift 2
    set +e
    tools/watchdog.sh "$seconds" "c03-$name" "$@" > "$logs/$name.log" 2>&1
    status=$?
    set -e
    printf '%s\t%s\n' "$name" "$status" >> "$out/exits.tsv"
    test "$status" -eq 0 || exit "$status"
}
run artifact-identity 1800 "$python" "$probe" identity --out "$out"
run reference-short 1800 "$python" "$probe" reference-short --out "$out"
run reference-long 1800 "$python" "$probe" reference-long --out "$out"
run baseline 900 "$python" "$probe" runtime --out "$out" --label baseline --server "$root/build/llama/bin/llama-server"
run candidate 900 "$python" "$probe" runtime --out "$out" --label candidate --server "$scratch/build/bin/llama-server"
run compare 120 "$python" "$probe" compare --out "$out"
