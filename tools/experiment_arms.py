#!/usr/bin/env python3
"""Pin and record every experiment arm a measurement runs under (AUDIT-004 F4).

The product reads its experiment flags once from the environment (src/core/experiments.c). A flag left unset takes
the product's current default, so the same command line changes arm when a default flips (SCRIBODO_ANSWER_PIPELINE,
on since 2026-11-14). Measurements therefore set every flag explicitly, stating defaults as values, and record them.

The flag list is read from src/core/experiments.c itself, so a new flag is pinned without editing this file:
  flag_on("X")        default on: unset is pinned as X=1 (off only for "off" or "0")
  flag_is("X", "v")   default off: unset is pinned as X=0 (or X=off when the on value is not "1")
  getenv("X") != NULL a debug switch any value turns on: never pinned (setting it would turn it on), recorded unset

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


def arms(source=SOURCE):
    """[(variable, pinned default or None)] in source order."""
    text = Path(source).read_text()
    body = text[text.index("read_environment"):]
    found = {}
    for kind, var, value in re.findall(r'(flag_on|flag_is|getenv)\("(SCRIBODO_[A-Z0-9_]+)"(?:,\s*"([^"]*)")?\)', body):
        found.setdefault(var, []).append((kind, value))
    out = []
    for var, uses in found.items():
        kinds = {k for k, _ in uses}
        if "flag_on" in kinds:
            default = "1"
        elif "flag_is" in kinds:
            default = "0" if all(v == "1" for k, v in uses if k == "flag_is") else "off"
        else:
            default = None
        out.append((var, default))
    if not out:
        raise SystemExit("experiment_arms: no flags found in %s" % source)
    return out


def pin(env, source=SOURCE):
    """A copy of env with every experiment flag set: a caller's value is kept, an unset one gets its default."""
    env = dict(env)
    for var, default in arms(source):
        if var not in env and default is not None:
            env[var] = default
    return env


def record(env, source=SOURCE):
    """{variable: value, or None when unset} for every experiment flag."""
    return {var: env.get(var) for var, _ in arms(source)}


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
