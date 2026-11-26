#!/usr/bin/env python3
"""C02 negative controls. Product sources stay untouched; only synthetic fixtures run.

Each source replacement must match exactly once. A killed mutant needs assertion
failure / exit 1, never a compiler error, timeout, sanitizer finding or parent crash.
Generated sources/binaries stay in a unique approved temporary directory; commands,
hashes, build logs and run logs are retained alongside this script.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[3]
    output = Path(__file__).resolve().parent / "mutation-evidence"
    output.mkdir(exist_ok=True)
    scratch = Path(tempfile.mkdtemp(
        prefix="scribodo-f2-mutations-",
        dir="/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode"))
    originals = {name: (root / f"src/app/{name}.c").read_text() for name in ("revisions", "renames")}
    variants = [
        ("control", []),
        ("first_reader_replay_disabled", [
            ("revisions", "!recover_move(project, NULL, NULL, false, NULL, NULL)", "false")]),
        ("inode_guard_removed", [
            ("revisions", "(uint64_t)st->st_dev == id->device && (uint64_t)st->st_ino == id->inode", "true")]),
        ("duplicate_log_publication", [
            ("renames", "if (found) {", "if (false && found) {")]),
        ("trailing_receipt_accepted", [
            ("revisions", "cJSON *j = json_parse_complete(text, strlen(text));", "cJSON *j = json_parse(text);")]),
        ("hardlinks_admitted", [
            ("revisions", "S_ISREG(st->st_mode) && st->st_nlink == 1", "S_ISREG(st->st_mode)"),
            ("revisions", "!S_ISREG(st.st_mode) || st.st_nlink != 1", "!S_ISREG(st.st_mode)"),
            ("revisions", "S_ISREG(a.st_mode) && a.st_nlink == 1", "S_ISREG(a.st_mode)")]),
    ]
    results = []
    for name, replacements in variants:
        sources = dict(originals)
        for module, before, after in replacements:
            if sources[module].count(before) != 1:
                raise RuntimeError(f"Replacement drift in {name}: {before}")
            sources[module] = sources[module].replace(before, after, 1)
        case = scratch / name
        case.mkdir()
        for module, text in sources.items():
            (case / f"{module}.c").write_text(text)
        binary = case / "test_revision_recovery"
        overlay = case / "overlay.mk"
        overlay.write_text(
            ".PHONY: c02-recovery-mutation\n"
            "c02-recovery-mutation: $(MARIADB_LIB)\n"
            "\t$(CC) $(CFLAGS) $(INCLUDES) -Isrc/app -Itests -DSCRIBODO_CRASH_POINTS "
            "$(filter-out src/app/revisions.c src/app/renames.c,$(REVISION_RECOVERY_SRCS)) "
            f'"{case}/revisions.c" "{case}/renames.c" -o "{binary}" $(MARIADB_LIBS) -lpthread\n')
        command = ["make", "--no-print-directory", "-f", "Makefile", "-f", str(overlay), "c02-recovery-mutation"]
        with (output / f"{name}-build.log").open("w") as log:
            built = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        if built.returncode:
            raise RuntimeError(f"Build failure is not a killed mutant: {name}")
        with (output / f"{name}-run.log").open("w") as log:
            ran = subprocess.run(
                ["tools/watchdog.sh", "300", f"c02-{name}", str(binary)], cwd=root,
                stdout=log, stderr=subprocess.STDOUT, timeout=320,
                env={**os.environ, "TMPDIR": str(scratch) + "/"})
        text = (output / f"{name}-run.log").read_text()
        expected = 1 if replacements else 0
        if ran.returncode != expected or (replacements and "FAIL " not in text):
            raise RuntimeError(f"Invalid control result {name}: exit {ran.returncode}")
        if any(marker in text for marker in ("AddressSanitizer", "UndefinedBehaviorSanitizer", "runtime error:")):
            raise RuntimeError(f"Runtime defect is not a killed mutant: {name}")
        result = {
            "name": name, "build_exit": built.returncode, "run_exit": ran.returncode,
            "command": command, "scratch": str(case),
            "source_sha256": {module: hashlib.sha256(source.encode()).hexdigest()
                              for module, source in sources.items()},
        }
        results.append(result)
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"{'KILLED' if replacements else 'PASS'} {name}: exit {ran.returncode}", flush=True)


if __name__ == "__main__":
    main()
