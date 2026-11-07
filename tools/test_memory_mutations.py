#!/usr/bin/env python3
"""Check that history regressions detect five weakened selection protocols.

Only generated files under build/memory-mutations change. No databases, models
or network are used. Compilation failures are errors, not killed mutations.
"""

import os
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    output = root / "build/memory-mutations"
    output.mkdir(parents=True, exist_ok=True)
    source = (root / "src/llm/chat_history.c").read_text()
    guard = """if (current->role != CHAT_USER || current->error || current->streaming || current->excerpt || !current->content ||
        (remote && !(current->via && current->via[0]))) return false;"""
    variants = {
        "control": source,
        "missing_current_guard": source.replace(guard, "if (false) return false;", 1),
        "local_text_to_remote": source.replace(
            "if (m->error || (remote && !(m->via && m->via[0]))) continue;", "if (m->error) continue;", 1
        ),
        "unbounded_scan": source.replace("scanned < CHAT_HISTORY_MAX_SCAN", "scanned < SIZE_MAX", 1),
        "missing_byte_guard": source.replace("if (len > room)", "if (false)", 1),
        "orphaned_assistant": source.replace(
            "while (count && msgs[indices[count - 1]].role != CHAT_USER)", "while (false)", 1
        ),
    }
    compiler = shlex.split(os.environ.get("CC", "cc"))
    for name, text in variants.items():
        if name != "control" and text == source:
            raise RuntimeError(f"Mutation no longer matches source: {name}")
        path = output / f"{name}.c"
        path.write_text(text)
        binary = output / name
        with (output / f"{name}.log").open("w") as log:
            subprocess.run(
                [*compiler, "-std=c11", "-O1", "-g", "-Isrc", "-Isrc/llm", "-Itests",
                 str(path), "tests/test_chat_history.c", "-o", str(binary)],
                cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60,
            )
            result = subprocess.run([str(binary)], cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        if name == "control":
            if result.returncode:
                raise RuntimeError("Unmodified control failed")
            print("PASS unmodified control")
        else:
            if result.returncode == 0:
                raise RuntimeError(f"Mutation survived: {name}")
            print(f"KILLED {name}")


if __name__ == "__main__":
    main()
