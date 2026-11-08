#!/bin/bash
# Selection-only reach measurement on a realistic synthetic manuscript
# (MEMORY-015 S2). Real tokenizer, SQL, Qdrant and planner; no generation.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
export SCRIBODO_LLAMA_SERVER="${SCRIBODO_LLAMA_SERVER:-$PWD/build/llama/bin/llama-server}"
mkdir -p build/memory-reach
make --no-print-directory -f Makefile -f - memory-reach-build <<'MAKE'
REACH_OBJS := $(sort $(CORE_OBJS) $(LLM_OBJS))
.PHONY: memory-reach-build
memory-reach-build: $(REACH_OBJS) $(LLAMA_BIN)
	$(CC) $(CFLAGS) $(INCLUDES) tools/memory_reach_eval.c $(REACH_OBJS) -o build/memory-reach/memory_reach_eval $(MARIADB_LIBS) -lm -lpthread
MAKE
if [[ "${1-}" == "--build-only" ]]; then exit 0; fi
exec python3 tools/memory_reach_eval.py "$@"
