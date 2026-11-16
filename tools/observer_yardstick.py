#!/usr/bin/env python3
"""The observer yardstick, checked against a tracked record (AUDIT-004 F9).

usage: tools/observer_yardstick.py [--record] [--binary build/observer_replay] [--expected FILE]

Runs tools/observer_replay_run.sh with every observer flag off (the default path) over the three public-domain test
novels and compares each novel's sorted findings with tests/fixtures/observer_yardstick.json: the line count and the
SHA-256 of the output, and the SHA-256 of the input chapters, so a changed input is reported as a changed input and
not as a regression. Until 2026-11-15 the expected counts (717 / 348 / 948) lived only in journal prose.

--record rewrites the expected file from this run; do it only for a change whose default-path effect is measured and
journaled, and say so in the commit.

The novels are untracked (build/manuscripts, from Project Gutenberg; MEMORY-021). Without them the check fails, except
on a GitHub-hosted runner (CI and RUNNER_ENVIRONMENT=github-hosted) or with SCRIBODO_YARDSTICK_SKIP set, where it
skips with a message, the same rule as tools/evidence_check.py.
Exit 0 when every novel matches; 1 on a mismatch; 2 when it cannot run."""
import argparse
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LABEL = "yardstick-check"
# The input folders observer_replay_run.sh reads for each novel.
INPUTS = {"pp": "build/manuscripts/pride-and-prejudice/parts",
          "mars": "build/manuscripts/a-princess-of-mars",
          "heights": "build/manuscripts/wuthering-heights"}


def sha_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def sha_inputs(folder):
    """One hash over the folder's chapter files in name order, each name and content."""
    h = hashlib.sha256()
    for name in sorted(n for n in os.listdir(folder) if n.endswith(".md")):
        h.update(name.encode() + b"\0")
        with open(os.path.join(folder, name), "rb") as f:
            h.update(f.read())
        h.update(b"\0")
    return h.hexdigest()


def may_skip():
    hosted = os.environ.get("CI") and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted"
    return bool(hosted or os.environ.get("SCRIBODO_YARDSTICK_SKIP"))


def run(binary):
    env = dict(os.environ, SCRIBODO_OBSERVER_CONFLICT_GATE="off", SCRIBODO_OBSERVER_SENTENCES="off")
    env.pop("SCRIBODO_FAKE_EXTRACT_MS", None)
    r = subprocess.run([os.path.join(ROOT, "tools/observer_replay_run.sh"), LABEL, binary], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        return None
    got = {}
    for novel, folder in INPUTS.items():
        out = os.path.join(ROOT, "build/observer", f"{LABEL}-{novel}.txt")
        with open(out, "rb") as f:
            data = f.read()
        got[novel] = {"lines": data.count(b"\n"), "sha256": hashlib.sha256(data).hexdigest(),
                      "inputs_sha256": sha_inputs(os.path.join(ROOT, folder))}
    return got


def compare(expected, got):
    """The mismatch lines, empty when every novel matches."""
    problems = []
    for novel in INPUTS:
        e, g = expected["novels"].get(novel), got.get(novel)
        if e is None or g is None:
            problems.append(f"{novel}: missing from {'the record' if e is None else 'this run'}")
            continue
        if e["inputs_sha256"] != g["inputs_sha256"]:
            problems.append(f"{novel}: the input chapters differ from the recorded ones; the yardstick measures nothing "
                            "until they are restored (build/manuscripts, MEMORY-021)")
            continue
        if e["lines"] != g["lines"] or e["sha256"] != g["sha256"]:
            problems.append(f"{novel}: {g['lines']} lines, sha256 {g['sha256'][:12]}; recorded {e['lines']} lines, "
                            f"sha256 {e['sha256'][:12]} (output build/observer/{LABEL}-{novel}.txt)")
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build/observer_replay"))
    ap.add_argument("--expected", default=os.path.join(ROOT, "tests/fixtures/observer_yardstick.json"))
    a = ap.parse_args()
    missing = [f for f in INPUTS.values() if not os.path.isdir(os.path.join(ROOT, f))]
    if missing:
        msg = f"observer_yardstick: the test novels are absent ({', '.join(missing)})"
        if may_skip():
            print(msg + "; skipped (hosted runner or SCRIBODO_YARDSTICK_SKIP)")
            return 0
        print(msg + "; set SCRIBODO_YARDSTICK_SKIP=1 to skip deliberately", file=sys.stderr)
        return 2
    if not os.access(a.binary, os.X_OK):
        print(f"observer_yardstick: no replay binary at {a.binary} (make tools)", file=sys.stderr)
        return 2
    got = run(a.binary)
    if got is None:
        print("observer_yardstick: the replay run failed", file=sys.stderr)
        return 2
    if a.record:
        with open(a.expected, "w", encoding="utf-8") as f:
            json.dump({"about": "tools/observer_yardstick.py: the observer's default-path replay output per novel "
                                "(flags off, fake backend); rewrite only with --record for a measured, journaled change",
                       "novels": got}, f, indent=1, sort_keys=True)
            f.write("\n")
        print("observer_yardstick: recorded " + ", ".join(f"{n} {g['lines']}" for n, g in got.items()))
        return 0
    with open(a.expected, encoding="utf-8") as f:
        expected = json.load(f)
    problems = compare(expected, got)
    for p in problems:
        print("observer_yardstick: " + p, file=sys.stderr)
    if problems:
        return 1
    print("observer_yardstick: " + ", ".join(f"{n} {g['lines']}" for n, g in got.items()) + " lines, byte-identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
