#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
make --no-print-directory -f Makefile -f - native-memory-build <<'MAKE'
NATIVE_OBJS := $(sort $(CORE_OBJS) $(filter-out $(BUILD)/src/llm/chat.o,$(LLM_OBJS)))
.PHONY: native-memory-build
native-memory-build: $(NATIVE_OBJS) $(LLAMA_BIN)
	$(CC) $(CFLAGS) $(INCLUDES) tools/memory_native_eval.c $(NATIVE_OBJS) -o build/memory_native_eval $(MARIADB_LIBS) -lpthread -lm
MAKE
if [[ "${1-}" == "--build-only" ]]; then exit 0; fi
exec python3 tools/memory_native_eval.py "$@"
