#!/usr/bin/env python3
"""Compile the retained native C02 prototype and assertion-based negative controls.

Only writes to the supplied new output directory and each executable's mkdtemp fixture.
Compiler errors, sanitizer findings, crashes and timeouts never count as killed mutants.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tmpdir", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name("key_probe.c").read_text()
    variants = [
        ("control", None),
        ("directory_symlink_followed",
         ("O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC", "O_DIRECTORY | O_CLOEXEC")),
        ("truncated_path_exposed",
         ("out[0] = '\\0';\n    return false;\n}", "return false;\n}")),
        ("snapshot_bytes_unchecked",
         (" && !memcmp(text, got, sizeof(text) - 1)", "")),
        ("owner_bytes_unchecked",
         (" && !memcmp(name, owner, n)", "")),
        ("noncanonical_uppercase_accepted",
         ("return -1; /* reject noncanonical uppercase too */",
          "if (c >= 'A' && c <= 'F') return c - 'A' + 10;\n    return -1;")),
    ]
    rows = []
    for name, mutation in variants:
        text = source
        if mutation:
            old, new = mutation
            if text.count(old) != 1:
                raise RuntimeError(f"{name}: expected exactly one mutation site, got {text.count(old)}")
            text = text.replace(old, new)
        generated = output / (name + ".c")
        generated.write_text(text)
        binary = output / name
        command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Wshadow",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-fno-sanitize-recover=undefined", str(generated), "-o", str(binary)]
        with (output / (name + "-build.log")).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120, check=True)
        env = dict(os.environ, TMPDIR=str(args.tmpdir.resolve()))
        result = subprocess.run([str(binary)], env=env, text=True, capture_output=True, timeout=120)
        raw = result.stdout + result.stderr
        (output / (name + ".log")).write_text(raw)
        summary = re.search(r"(\d+) checks, (\d+) failures;", raw)
        if not summary or "AddressSanitizer" in raw or "runtime error:" in raw:
            raise RuntimeError(f"{name}: missing assertion summary or sanitizer finding, exit {result.returncode}")
        failures = int(summary[2])
        if mutation:
            if result.returncode != 1 or not failures:
                raise RuntimeError(f"{name}: mutant survived or crashed, exit {result.returncode}")
        elif result.returncode or failures:
            raise RuntimeError(f"{name}: unmodified control failed")
        row = {"name": name, "checks": int(summary[1]), "failures": failures,
               "exit": result.returncode, "result": "killed" if mutation else "passed"}
        rows.append(row)
        print(json.dumps(row), flush=True)
    (output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
