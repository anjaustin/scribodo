#!/bin/bash
# Real local pinned-model quality experiment, NOT the fake pipeline soak.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
export SCRIBODO_LLAMA_SERVER="${SCRIBODO_LLAMA_SERVER:-$PWD/build/llama/bin/llama-server}"
build_only=0
if [[ "${1-}" == "--build-only" ]]; then
    [[ $# == 1 ]] || { echo '--build-only must be used alone' >&2; exit 2; }
    build_only=1
fi
if [[ "${1-}" == "--help" || "${1-}" == "--self-test" || "${1-}" == "--prepare-only" ]]; then
    exec python3 tools/memory_quality_eval.py "$@"
fi
mkdir -p build/memory-quality
# The Makefile rule (make tools builds it too; AUDIT-004 F9).
make --no-print-directory build/memory-quality/memory_quality_eval llama
python3 tools/memory_quality_eval.py --snapshot-build
if (( build_only )); then exit 0; fi
exec python3 tools/memory_quality_eval.py "$@"
