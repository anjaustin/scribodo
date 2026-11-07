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
# Temporary overlay only. Parent may already have added these modules to its
# object lists; sort avoids duplicate definitions without editing the Makefile.
make --no-print-directory -f Makefile -f - memory-quality-build <<'MAKE'
QUALITY_EXTRA := build/src/app/memory_brief_store.o build/src/llm/memory_brief.o build/src/llm/generative_gate.o
QUALITY_OBJS := $(sort $(CORE_OBJS) $(LLM_OBJS) $(QUALITY_EXTRA))
.PHONY: memory-quality-build
memory-quality-build: $(QUALITY_OBJS) $(LLAMA_BIN)
	$(CC) $(CFLAGS) $(INCLUDES) tools/memory_quality_eval.c $(QUALITY_OBJS) -o build/memory-quality/memory_quality_eval $(MARIADB_LIBS) -lm -lpthread
MAKE
python3 tools/memory_quality_eval.py --snapshot-build
if (( build_only )); then exit 0; fi
exec python3 tools/memory_quality_eval.py "$@"
