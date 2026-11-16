#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
# The Makefile rule (make tools builds it too; AUDIT-004 F9).
make --no-print-directory build/memory_native_eval llama
if [[ "${1-}" == "--build-only" ]]; then exit 0; fi
exec python3 tools/memory_native_eval.py "$@"
