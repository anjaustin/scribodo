#!/bin/bash
# make remotetest: the optional remote model in the app, against a stand-in
# provider on 127.0.0.1 (tests/remote/fake_remote.py). Checks the UI flow
# and what actually reached the provider.
set -u
cd "$(dirname "$0")/.."
out=$PWD/build/ui/remote
chmod -R u+w "$out" 2>/dev/null; rm -rf "$out"; mkdir -p "$out/work"
fail=0
key="sk-test-$$"
printf '%s' "$key" > "$out/key"; chmod 600 "$out/key"
python3 -I tests/remote/fake_remote.py "$out/port" "$out/requests.jsonl" "$key" &
srv=$!
trap 'kill $srv 2>/dev/null' EXIT
for i in $(seq 50); do [ -s "$out/port" ] && break; sleep 0.1; done
port=$(cat "$out/port")
printf '{"name":"Frontier","base_url":"http://127.0.0.1:%s/v1","model":"m1","api_key_file":"%s"}' "$port" "$out/key" > "$out/remote.json"

SCRIBODO_REMOTE_CONFIG=$out/remote.json SCRIBODO_SCRIPT=tests/remote/remote.scribo SCRIBODO_SCRIPT_DIR=$out/work \
    SCRIBODO_SHOT_DIR=$out tools/watchdog.sh 120 remote ./build/scribodo 2>"$out/stderr.log" || fail=1
echo
SCRIBODO_REMOTE_CONFIG=$out/none.json SCRIBODO_SCRIPT=tests/remote/unconfigured.scribo SCRIBODO_SCRIPT_DIR=$out/work \
    SCRIBODO_SHOT_DIR=$out tools/watchdog.sh 120 remote ./build/scribodo 2>>"$out/stderr.log" || fail=1
echo
# What reached the provider: exactly the one remote message, with its key,
# model and attached document; nothing after switching back, no observer.
python3 -I - "$out/requests.jsonl" "$out/work" <<'PY' || fail=1
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    return ok
b = json.loads(reqs[0]["body"]) if reqs else {}
allc = " ".join(m["content"] for m in b.get("messages", []))
ok = all([
    check(len(reqs) == 1, "one request reached the provider (got %d)" % len(reqs)),
    check(bool(reqs) and reqs[0]["path"] == "/v1/chat/completions" and reqs[0]["auth_ok"], "chat completions, with the key"),
    check(b.get("model") == "m1" and "id_slot" not in b, "the configured model; no local-only fields"),
    check("hello remote" in allc and "The ferry runs on Tuesdays." in allc, "the message and its attached document"),
    check("now local" not in " ".join(r["body"] for r in reqs), "nothing sent after switching back to local"),
    check("PRIVATE" not in " ".join(r["body"] for r in reqs), "the local exchange before the switch was never sent"),
])
# The saved chat says which messages went to the remote model.
import glob
logs = glob.glob(sys.argv[2] + "/rem/.scribodo/chats/*.jsonl")
msgs = [json.loads(l) for l in open(logs[0])] if logs else []
msgs = [m for m in msgs if m.get("type") == "message"]
via = [(m["role"], m.get("via", "")) for m in msgs]
ok = check(len(msgs) == 6 and [v for _, v in via] == [""] * 2 + ["Frontier \u00b7 127.0.0.1"] * 2 + [""] * 2,
           "saved: the remote pair marked, the local pair not (%s)" % via) and ok
sys.exit(0 if ok else 1)
PY
exit $fail
