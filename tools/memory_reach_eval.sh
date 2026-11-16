#!/bin/bash
# Selection-only reach measurement on a realistic synthetic manuscript
# (MEMORY-015 S2). Real tokenizer, SQL, Qdrant and planner; no generation.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
export SCRIBODO_LLAMA_SERVER="${SCRIBODO_LLAMA_SERVER:-$PWD/build/llama/bin/llama-server}"
mkdir -p build/memory-reach
# The Makefile rule (make tools builds it too; AUDIT-004 F9).
make --no-print-directory build/memory-reach/memory_reach_eval llama
if [[ "${1-}" == "--build-only" ]]; then exit 0; fi
exec python3 tools/memory_reach_eval.py "$@"
