#!/bin/bash
# Synthetic/public-domain fixtures only. Database gates and shared yardsticks run serially.
# Usage: bash run-final-gates.sh [log-prefix] [all|ui] [evidence-directory]
set -u
out=${3:-journal/composition001/c02}
tag=${1:-final}
mode=${2:-all}
case "$mode" in all|ui) ;; *) exit 2 ;; esac
mkdir -p "$out" || exit 2
status=0
: > "$out/$tag-gates.tsv"
gate() {
    local name=$1 code
    shift
    "$@" > "$out/$tag-$name.log" 2>&1
    code=$?
    printf '%s\t%d\n' "$name" "$code" >> "$out/$tag-gates.tsv"
    if [ "$code" -ne 0 ]; then status=1; fi
}
isolated() {
    env SCRIBODO_SERVICES=native SCRIBODO_DB_PORT=13471 SCRIBODO_QDRANT_PORT=13472 \
        SCRIBODO_SECRETS=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-f2.bPW080/secrets \
        SCRIBODO_LOGS=/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-f2.bPW080/logs "$@"
}
gate services-up isolated tools/services.sh up
if [ "$mode" = all ]; then
gate test env SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make -j1 test
gate sanitize env SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make -j1 sanitize
gate analyze make -j1 analyze
gate tsan make -j1 tsan
gate dbtest isolated make -j1 dbtest
gate crashtest isolated make -j1 crashtest
fi
gate uitest env SCRIBODO_STORE=off make -j1 uitest
gate cstest env SCRIBODO_STORE=off make -j1 cstest
gate uitest-db isolated make -j1 uitest-db
gate services-down isolated tools/services.sh down
cat "$out/$tag-gates.tsv"
exit "$status"
