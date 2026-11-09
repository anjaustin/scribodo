#!/usr/bin/env python3
"""Local product regression controls. Requires the test DB services, not models.

Only generated sources under build/ are changed. Compile errors, crashes and
timeouts are errors, never successful mutation detections.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def command(target, changed):
    dry = subprocess.check_output(["make", "--no-print-directory", "-n", "-W", changed, target], cwd=ROOT, text=True)
    commands = [shlex.split(line) for line in dry.splitlines()]
    matches = [args for args in commands if "-o" in args and args[args.index("-o") + 1] == target]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one compiler command for {target}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/product-memory-mutations')
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    subprocess.run(["make", "build/test_memory_chat", "build/test_persistence_faults"], cwd=ROOT, check=True)
    compile_chat = command("build/src/llm/chat.o", "src/llm/chat.c")
    link_chat = command("build/test_memory_chat", "src/llm/chat.c")
    fault = command("build/test_persistence_faults", "tests/test_persistence_faults.c")
    source = (ROOT / "src/llm/chat.c").read_text()
    publication = "char *out = json_ok ? cJSON_PrintUnformatted(root) : NULL;"
    if source.count(publication) != 2:
        raise RuntimeError("Expected chat and edit publication guards")
    variants = [
        ("history_control", "history", None),
        ("copied_tail_excluded", "history", ("for (size_t i = first_history; i < history.count; i++)",
                                                "for (size_t i = 0; i < history.count; i++)")),
        ("recall_reservation_removed", "history", ("base_ctx -= (int)MEMORY_PLAN_TOKENS;", "base_ctx -= 0;")),
        ("payload_control", "fault", None),
        ("chat_partial_publication", "fault", 0),
        ("edit_partial_publication", "fault", 1),
    ]
    results = []
    for name, suite, mutation in variants:
        text = source
        if isinstance(mutation, tuple):
            old, new = mutation
            if text.count(old) != 1:
                raise RuntimeError(f"Mutation no longer matches: {name}")
            text = text.replace(old, new)
        elif isinstance(mutation, int):
            locations = [m.start() for m in re.finditer(re.escape(publication), text)]
            at = locations[mutation]
            text = text[:at] + "char *out = cJSON_PrintUnformatted(root);" + text[at + len(publication):]
        generated, binary = output / f"{name}.c", output / name
        generated.write_text(text)
        if suite == "history":
            obj = output / f"{name}.o"
            build = [str(generated) if arg == "src/llm/chat.c" else arg for arg in compile_chat]
            build[build.index("-o") + 1] = str(obj)
            build.insert(1, "-Isrc/llm")
            link = [str(obj) if arg == "build/src/llm/chat.o" else arg for arg in link_chat]
            link[link.index("-o") + 1] = str(binary)
            commands = [build, link]
        else:
            build = [str(generated) if arg == "src/llm/chat.c" else arg for arg in fault]
            build[build.index("-o") + 1] = str(binary)
            build.insert(1, "-Isrc/llm")
            commands = [build]
        with (output / f"{name}-build.log").open("w") as log:
            for args in commands:
                subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
        result = subprocess.run(["tools/watchdog.sh", "180", name, str(binary)], cwd=ROOT,
                                capture_output=True, text=True, timeout=200)
        log = result.stdout + result.stderr
        (output / f"{name}.log").write_text(log)
        summary = re.search(r"(\d+) checks, (\d+) failures", log)
        if not summary:
            raise RuntimeError(f"Missing normal test summary: {name}, exit {result.returncode}")
        failures = int(summary.group(2))
        if mutation is None:
            if result.returncode or failures:
                raise RuntimeError(f"Unchanged control failed: {name}")
        elif result.returncode != 1 or failures == 0:
            raise RuntimeError(f"Mutation survived or errored: {name}")
        row = {"name": name, "result": "passed" if mutation is None else "killed",
               "checks": int(summary.group(1)), "failures": failures}
        results.append(row)
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(json.dumps(row), flush=True)


if __name__ == "__main__":
    main()
