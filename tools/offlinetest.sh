#!/bin/bash
# make offlinetest: SOW acceptance 8, "without any network connection".
# Everything runs inside a macOS sandbox that denies all network access
# except to this machine (tools/offline/no-network.sb): the app, both
# llama-servers and their children. The Mac's own network stays up.
#   1. Controls: inside the sandbox an outside host is unreachable (and
#      reachable outside it), while loopback works.
#   2. The store suite (fake model), then the real LFM3 and Qwen3-Embedding
#      models through the UI, on files and on the store, inside it.
#   3. The real model end to end (make llmtest) inside it.
# Needs the real model: don't run it while anything else uses the model.
set -u
cd "$(dirname "$0")/.."
SB=$PWD/tools/offline/no-network.sb
sb() { sandbox-exec -f "$SB" "$@"; }
fail=0
check() { if [ "$1" = 0 ]; then echo "  ok    $2"; else echo "  FAIL  $2"; fail=1; fi; }
out=$PWD/build/ui/offline
chmod -R u+w "$out" 2>/dev/null; rm -rf "$out"; mkdir -p "$out"

echo "controls"
code=$(sb /usr/bin/curl -s -m 8 -o /dev/null -w '%{http_code}' https://example.com 2>/dev/null); [ "$code" = 000 ]; check $? "an outside host is unreachable inside the sandbox"
code=$(/usr/bin/curl -s -m 8 -o /dev/null -w '%{http_code}' https://example.com 2>/dev/null); [ "$code" != 000 ]; check $? "... and reachable outside it (the network is up)"
code=$(sb /usr/bin/curl -s -m 8 -o /dev/null -w '%{http_code}' http://127.0.0.1:6333/ 2>/dev/null); [ "$code" = 200 ]; check $? "loopback works inside it"


echo "the store suite, fake model"
db=scribodo_test_off$$
tools/services.sh testdb create $db >/dev/null
work=$out/store; mkdir -p "$work"
for s in 1_move_and_use 2_resume 6_backup 5_entities; do
    env SCRIBODO_STORE=db SCRIBODO_DB_NAME=$db SCRIBODO_BACKUPS=$work/backups SCRIBODO_SCRIPT=tests/ui/store/$s.scribo \
        SCRIBODO_SCRIPT_DIR=$work SCRIBODO_SHOT_DIR=$out tools/watchdog.sh 300 "offline-$s" sandbox-exec -f "$SB" ./build/scribodo \
        2>>"$out/stderr.log" | grep -E "^script|passed|FAIL"
    check "${PIPESTATUS[0]}" "store run $s"
done
tools/services.sh testdb drop $db >/dev/null

echo "the real models"
for s in real-chat real-edit real-observer; do
    mkdir -p "$out/$s"; cp -R tests/ui/fixtures/. "$out/$s/"
    env SCRIBODO_LLM=real SCRIBODO_SEED=42 SCRIBODO_SCRIPT=tests/ui/$s.scribo SCRIBODO_SCRIPT_DIR=$out/$s SCRIBODO_SHOT_DIR=$out \
        caffeinate -i tools/watchdog.sh 1800 "offline-$s" sandbox-exec -f "$SB" ./build/scribodo 2>>"$out/stderr.log" |
        grep -E "^script|passed|FAIL"
    check "${PIPESTATUS[0]}" "$s (files)"
done
db=scribodo_test_off$$_r
tools/services.sh testdb create $db >/dev/null
mkdir -p "$out/real-store"
env SCRIBODO_LLM=real SCRIBODO_SEED=42 SCRIBODO_STORE=db SCRIBODO_DB_NAME=$db SCRIBODO_SCRIPT=tests/ui/real-observer.scribo \
    SCRIBODO_SCRIPT_DIR=$out/real-store SCRIBODO_SHOT_DIR=$out \
    caffeinate -i tools/watchdog.sh 1800 offline-real-store sandbox-exec -f "$SB" ./build/scribodo 2>>"$out/stderr.log" |
    grep -E "^script|passed|FAIL"
check "${PIPESTATUS[0]}" "real-observer on the store, with the real embedder"
tools/services.sh testdb drop $db >/dev/null

echo "the real model end to end"
SCRIBODO_LLAMA_SERVER=$PWD/build/llama/bin/llama-server sb tools/watchdog.sh 1200 offline-llm ./build/test_llm --live 2>&1 | grep -E "checks|FAIL"
check "${PIPESTATUS[0]}" "make llmtest's suite"
exit $fail
