#!/bin/bash
# Standalone DB diagnostic; never starts/stops services or opens a project.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
usage() {
    echo "usage: tools/history_bench.sh [quick|soak] [--iterations 1..100000]"
    echo "quick: 15 samples/case; soak: 500. Both seed 100/1000/10000 messages."
    echo "Needs tools/services.sh up and the existing MariaDB client build."
}
mode=quick
if [[ ${1:-} == --help ]]; then usage; exit 0; fi
if [[ ${1:-} == quick || ${1:-} == soak ]]; then mode=$1; shift; fi
iterations=15
[[ $mode != soak ]] || iterations=500
if [[ ${1:-} == --iterations && $# == 2 ]]; then iterations=$2; shift 2; fi
if [[ $# != 0 || ! $iterations =~ ^[1-9][0-9]{0,5}$ ]] || (( iterations > 100000 )); then
    usage >&2; exit 2
fi
port=${SCRIBODO_DB_PORT-33061}
if [[ ! $port =~ ^[1-9][0-9]{0,4}$ ]] || (( port > 65535 )); then
    echo 'invalid SCRIBODO_DB_PORT (expected 1..65535)' >&2; exit 2
fi
cd "$here"
lib=build/mariadb-cc/libmariadb/libmariadbclient.a
if [[ ! -f $lib ]]; then
    echo "Missing $lib; build the existing MariaDB client target first." >&2; exit 1
fi
mkdir -p build/history-bench
# Dead stripping keeps the real chat_store_free_messages implementation without
# pulling file/project adapters into this DB-only executable.
link=(-Wl,--gc-sections -lm -lpthread -ldl)
ssl=${OPENSSL_DIR:-/opt/homebrew/opt/openssl@3}
if [[ $(uname -s) == Darwin ]]; then link=(-Wl,-dead_strip -liconv -lm -lpthread); fi
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -O2 -g \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    -Isrc -Ithird_party/cJSON -Ithird_party/mariadb-connector-c/include -Ibuild/mariadb-cc/include \
    tools/history_bench.c src/app/db.c src/app/store_chat_history.c src/app/chat_store.c src/core/utf8.c \
    "$lib" "$ssl/lib/libssl.a" "$ssl/lib/libcrypto.a" -lz "${link[@]}" \
    -o build/history-bench/history_bench
name="scribodo_test_history_$$_$(openssl rand -hex 8)"
echo "history_bench: disposable_database=$name"
created=0
cleanup() {
    rc=$?
    trap - EXIT
    if (( created )); then
        if ! tools/services.sh testdb drop "$name"; then
            echo "Cleanup failed for $name; retry: tools/services.sh testdb drop $name" >&2
            [[ $rc != 0 ]] || rc=1
        else
            echo "history_bench: cleaned_database=$name"
        fi
    fi
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# Arm cleanup before creation: a failed GRANT can leave a newly created DB.
created=1
tools/services.sh testdb create "$name"
build/history-bench/history_bench "$name" "$mode" "$iterations"
