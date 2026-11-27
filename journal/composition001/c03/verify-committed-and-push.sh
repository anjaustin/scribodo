#!/bin/bash
# Verify the exact committed C03 checkpoint, archive all gates, then push only a clean unchanged HEAD.
# Synthetic/public-domain fixtures; model and shared-yardstick stages remain serial.
set -eu
scratch=${1:?Usage: verify-committed-and-push.sh SCRATCH NEW_OUTPUT EXPECTED_COMMIT}
out=${2:?New output required}
expected=${3:?Exact committed HEAD required}
test "$(git rev-parse HEAD)" = "$expected"
test -z "$(git status --porcelain)"
mkdir "$out"
bash journal/composition001/c03/run-compatibility.sh "$scratch" "$out/parity" > "$out/parity-driver.log" 2>&1
python3 journal/composition001/c03/verify_parity.py "$out/parity" > "$out/postflight.json" 2> "$out/postflight.log"
bash journal/composition001/c03/run-redteam-gates.sh "$scratch" "$out/gates" > "$out/gate-driver.log" 2>&1
python3 journal/composition001/c03/archive_parity.py --run "$out" --candidate-build "$scratch/build" \
    --name scribodo-evidence-2026-11-27-composition001-c03-committed01 > "$scratch/committed01-archive-receipt.json"
test "$(git rev-parse HEAD)" = "$expected"
test -z "$(git status --porcelain)"
git push origin main
remote=$(git ls-remote origin refs/heads/main | awk '{print $1}')
test "$remote" = "$expected"
printf 'Verified and pushed %s\n' "$expected"
