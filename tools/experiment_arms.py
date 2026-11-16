#!/usr/bin/env python3
"""Pin and record every experiment arm a measurement runs under (AUDIT-004 F4).

The product reads its experiment flags once from the environment (src/core/experiments.c). A flag left unset takes
the product's current default, so the same command line changes arm when a default flips (SCRIBODO_ANSWER_PIPELINE,
on since 2026-11-14). Measurements therefore set every flag explicitly, stating defaults as values, and record them.

The flag list is every "SCRIBODO_*" literal in src/core/experiments.c, so a new flag is pinned without editing this
file, and a literal of a kind it does not know stops the run (classify):
  flag_on("X")              default on: unset is pinned as X=1 (off only for "off" or "0")
  flag_is("X", "v")         default off: unset is pinned as X=0 (or X=off when the on value is not "1")
  getenv("X") + off_value   a level flag (the observer's): unset is pinned as X=off
  getenv("X") != NULL       a debug switch any value turns on: never pinned, recorded (unset when unset)
  positive_int("X")         a test hook (SCRIBODO_FAKE_EXTRACT_MS): never pinned, recorded when set

Shell:   eval "$(python3 tools/experiment_arms.py --export)"; python3 tools/experiment_arms.py --line
Python:  env = experiment_arms.pin(env); manifest["experiment_arms"] = experiment_arms.record(env)
"""
import argparse
import json
import os
import re
import shlex
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/core/experiments.c"


def enclosing_block(text, at):
    """The outermost {...} block of the source that contains position at (a function body), or None. Braces in
    string literals and comments are skipped."""
    depth, start = 0, None
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            i = text.find("*/", i + 2)
            i = n if i < 0 else i + 2
            continue
        if text.startswith("//", i):
            i = text.find("\n", i)
            i = n if i < 0 else i
            continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start is not None and start < at < i:
                return text[start:i + 1]
        i += 1
    return None


def classify(source=SOURCE):
    """[(variable, kind, pinned default or None)] for every "SCRIBODO_*" literal in the flag table's source, in order.

    kinds: "arm" (flag_on: pinned 1; flag_is: pinned 0, or off when its on value is not "1"), "level" (read with
    getenv inside a helper that tests off_value, as the observer's level flags are: pinned off), "switch"
    (getenv(...) != NULL, any value turns it on: never pinned, recorded) and "hook" (positive_int, a test hook:
    never pinned, recorded when set). A literal that fits none of these, or a variable with only mentions
    (warn_value's message), stops the run: a new kind of flag needs a decision here, not a silent drop."""
    text = Path(source).read_text()
    uses = {}
    unknown = []
    for m in re.finditer(r'"(SCRIBODO_[A-Z0-9_]+)"', text):
        var = m.group(1)
        before = text[max(0, m.start() - 16):m.start()]
        after = text[m.end():m.end() + 24]
        if before.endswith("flag_on("):
            uses.setdefault(var, []).append(("flag_on", None))
        elif before.endswith("flag_is("):
            value = re.match(r'\s*,\s*"([^"]*)"', after)
            uses.setdefault(var, []).append(("flag_is", value.group(1) if value else None))
        elif before.endswith("positive_int("):
            uses.setdefault(var, []).append(("hook", None))
        elif before.endswith("getenv(") and re.match(r"\)\s*!=\s*NULL", after):
            uses.setdefault(var, []).append(("switch", None))
        elif before.endswith("getenv("):
            # A level flag: "const char *v = getenv(X)" inside a function that tests off_value(v); the function is
            # bounded by brace matching, so an off_value call in a later helper does not count.
            assigned = re.search(r"(\w+)\s*=\s*getenv\($", text[max(0, m.start() - 64):m.start()])
            block = enclosing_block(text, m.start())
            if assigned and block and re.search(r"\boff_value\(\s*%s\s*\)" % re.escape(assigned.group(1)), block):
                uses.setdefault(var, []).append(("level", None))
            else:
                unknown.append(var)
        elif before.endswith("warn_value("):
            uses.setdefault(var, [])
        else:
            unknown.append(var)
    unknown += [v for v, u in uses.items() if not u]
    if unknown:
        raise SystemExit("experiment_arms: cannot classify %s in %s; add its kind to tools/experiment_arms.py"
                         % (", ".join(sorted(set(unknown))), source))
    out = []
    for var, u in uses.items():
        kinds = {k for k, _ in u}
        if "flag_on" in kinds:
            out.append((var, "arm", "1"))
        elif "flag_is" in kinds:
            out.append((var, "arm", "0" if all(v == "1" for k, v in u if k == "flag_is") else "off"))
        elif "level" in kinds:
            out.append((var, "level", "off"))
        elif "switch" in kinds:
            out.append((var, "switch", None))
        else:
            out.append((var, "hook", None))
    if not out:
        raise SystemExit("experiment_arms: no flags found in %s" % source)
    return out


def arms(source=SOURCE):
    """[(variable, pinned default or None)] in source order: None for a switch or a test hook, never pinned."""
    return [(var, default) for var, _, default in classify(source)]


def pin(env, source=SOURCE):
    """A copy of env with every experiment flag set: a caller's value is kept, an unset one gets its default."""
    env = dict(env)
    for var, default in arms(source):
        if var not in env and default is not None:
            env[var] = default
    return env


def record(env, source=SOURCE):
    """{variable: value, or None when unset} for every arm, level and switch, and each test hook that is set."""
    return {var: env.get(var) for var, kind, _ in classify(source) if kind != "hook" or var in env}


def line(env, source=SOURCE):
    return "ARMS " + " ".join("%s=%s" % (k, "unset" if v is None else v) for k, v in record(env, source).items())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--export", action="store_true", help="print export lines pinning every unset flag")
    ap.add_argument("--line", action="store_true", help="print one ARMS line of the current values")
    ap.add_argument("--json", type=Path, help="write the current values to this file")
    ap.add_argument("--source", type=Path, default=SOURCE)
    a = ap.parse_args(argv)
    if a.export:
        for var, value in pin(os.environ, a.source).items():
            if var.startswith("SCRIBODO_") and var in dict(arms(a.source)) and value is not None:
                print("export %s=%s" % (var, shlex.quote(value)))
    if a.line:
        print(line(os.environ, a.source))
    if a.json:
        a.json.write_text(json.dumps(record(os.environ, a.source), indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
