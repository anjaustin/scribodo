#!/usr/bin/env python3
"""Mutation controls for AUDIT-001. Generated sources/logs stay under build/.

Compile failures, crashes and timeouts are errors, not killed mutants. Requires
the normal persistence-fault test prerequisites; no services or models are used.
"""
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    output = root / "build/audit-mutations"
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(["make", "build/test_persistence_faults"], cwd=root, check=True)
    dry = subprocess.check_output(
        ["make", "--no-print-directory", "-n", "-W", "tests/test_persistence_faults.c",
         "build/test_persistence_faults"], cwd=root, text=True)
    commands = [shlex.split(line) for line in dry.splitlines()
                if "-o build/test_persistence_faults " in line]
    if len(commands) != 1:
        raise RuntimeError("Expected exactly one persistence-test compiler command")
    command = commands[0]
    variants = [
        ("control", "src/app/chat_store.c", None),
        ("chat_eof_hides_allocation_failure", "src/app/chat_store.c",
         ("cJSON_Delete(j); lines_ok = false; break;", "cJSON_Delete(j); break;", 2)),
        ("chat_title_not_restored", "src/app/chat_store.c",
         ('snprintf(c->title, sizeof(c->title), "%s", t->valuestring);', "(void)t;", 1)),
        ("chat_observer_flag_not_restored", "src/app/chat_store.c",
         ("c->observer_ignore = !cJSON_IsTrue(rd);", "c->observer_ignore = false;", 1)),
        ("rules_trailing_bytes_accepted", "src/app/observer_store.c",
         ("if (ok && n == 16) ok = fgetc(f) == EOF && !ferror(f);", "/* extra byte ignored */", 1)),
        ("rules_nonhex_accepted", "src/app/observer_store.c",
         ("for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)out[i])) return false;",
          "/* character validation removed */", 1)),
        ("rules_read_error_accepted", "src/app/observer_store.c",
         ("bool ok = !ferror(f);", "bool ok = true;", 1)),
    ]
    results = []
    for name, source, mutation in variants:
        text = (root / source).read_text()
        if mutation:
            old, new, count = mutation
            if text.count(old) != count:
                raise RuntimeError(f"Mutation no longer matches: {name}")
            text = text.replace(old, new)
        generated = output / f"{name}.c"
        generated.write_text(text)
        binary = output / name
        args = [str(generated) if arg == source else arg for arg in command]
        args[args.index("-o") + 1] = str(binary)
        args.insert(args.index("-o"), "-Isrc/app")
        with (output / f"{name}-build.log").open("w") as log:
            subprocess.run(args, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                           timeout=120, check=True)
        result = subprocess.run([str(binary)], cwd=root, capture_output=True,
                                text=True, timeout=60)
        log = result.stdout + result.stderr
        (output / f"{name}.log").write_text(log)
        summary = re.search(r"(\d+) checks, (\d+) failures", log)
        if not summary:
            raise RuntimeError(f"Missing test summary: {name}, exit {result.returncode}")
        failures = int(summary.group(2))
        if mutation:
            if result.returncode != 1 or not failures:
                raise RuntimeError(f"Mutation survived or crashed: {name}")
        elif result.returncode or failures:
            raise RuntimeError(f"Unmodified control failed: {name}")
        row = {"name": name, "result": "killed" if mutation else "passed",
               "checks": int(summary.group(1)), "failures": failures}
        results.append(row)
        print(json.dumps(row), flush=True)
    # Test the gate tests themselves in an isolated, generated source layout.
    # Source links are read-only inputs; only this copy of Makefile is mutated.
    gates = output / "analyzer-gates"
    (gates / "tests").mkdir(parents=True, exist_ok=True)
    (gates / "build").mkdir(exist_ok=True)
    shutil.copyfile(root / "tests/test_analyze.py", gates / "tests/test_analyze.py")
    for source in (root / "src").rglob("*"):
        if source.is_file():
            link = gates / source.relative_to(root)
            link.parent.mkdir(parents=True, exist_ok=True)
            if not link.is_symlink():
                link.symlink_to(source)
    third_party = gates / "third_party"
    if not third_party.is_symlink():
        third_party.symlink_to(root / "third_party", target_is_directory=True)
    makefile = (root / "Makefile").read_text()
    library = "MARIADB_LIB := build/mariadb-cc/libmariadb/libmariadbclient.a"
    if makefile.count(library) != 1:
        raise RuntimeError("Expected exactly one shared client-library definition")
    makefile = makefile.replace(
        library, f"MARIADB_LIB := {root / 'build/mariadb-cc/libmariadb/libmariadbclient.a'}")
    gate_mutations = [
        ("gate_control", None),
        ("gate_objective_c_omitted", ("-name '*.[cm]'", "-name '*.c'")),
        ("gate_diagnostics_ignored", (" || [ -s $(BUILD)/analyze.log ]", "")),
        ("gate_exit_status_ignored", ("[ $$status -ne 0 ] || ", "")),
        ("gate_stale_log_retained", ("2> $(BUILD)/analyze.log", "2>> $(BUILD)/analyze.log")),
    ]
    for name, mutation in gate_mutations:
        text = makefile
        if mutation:
            old, new = mutation
            if text.count(old) != 1:
                raise RuntimeError(f"Gate mutation no longer matches: {name}")
            text = text.replace(old, new)
        (gates / "Makefile").write_text(text)
        result = subprocess.run(["python3", "tests/test_analyze.py"], cwd=gates,
                                capture_output=True, text=True, timeout=120)
        log = result.stdout + result.stderr
        (output / f"{name}.log").write_text(log)
        ran = re.search(r"Ran (\d+) tests?", log)
        if not ran:
            raise RuntimeError(f"Missing gate-test summary: {name}")
        failure = re.search(r"FAILED \(failures=(\d+)\)", log)
        if mutation:
            if result.returncode != 1 or not failure:
                raise RuntimeError(f"Gate mutation survived or errored: {name}")
        elif result.returncode:
            raise RuntimeError(f"Gate control failed: {name}; see log")
        row = {"name": name, "result": "killed" if mutation else "passed",
               "tests": int(ran.group(1)), "failures": int(failure.group(1)) if failure else 0}
        results.append(row)
        print(json.dumps(row), flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
