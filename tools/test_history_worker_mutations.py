#!/usr/bin/env python3
"""Headless concurrency controls; generated mutants stay under build/.

Compilation failures are errors, not killed mutants. No DB/model/services used.
The connection-lock mutant is terminated by the test's explicit SIGALRM gate.
"""
import os
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    output = root / "build/history-worker-mutations"
    output.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(os.environ.get("CC", "cc"))
    worker = (root / "src/app/chat_history_job.c").read_text()
    store = (root / "src/app/store.c").read_text()
    variants = [
        ("worker_control", worker, "worker", None),
        ("worker_stale_result", worker, "worker", (" && request.generation == job->generation", "")),
        ("worker_cancel_admission", worker, "worker", ("job->pending = job->wanted = false;", "job->pending = false;")),
        ("store_control", store, "store", None),
        ("store_connect_lock", store, "store", (
            "Db *opened = bounded ? db_open_until(&config, cancel, deadline, err, sizeof(err)) : db_open(&config, err, sizeof(err));",
            "pthread_mutex_lock(&manager->mutex); Db *opened = bounded ? db_open_until(&config, cancel, deadline, err, sizeof(err)) : db_open(&config, err, sizeof(err)); pthread_mutex_unlock(&manager->mutex);")),
        ("store_stale_generation", store, "store", (
            "generation == atomic_load(&manager->generation) &&", "")),
        ("store_marker_fallback", store, "store", (
            "if (lstat(path, &st) == 0 || errno != ENOENT) return STORE_PAUSED;", "if (false) return STORE_PAUSED;")),
    ]
    for name, source, kind, replacement in variants:
        text = source
        if replacement:
            if replacement[0] not in text:
                raise RuntimeError(f"Mutation no longer matches: {name}")
            text = text.replace(*replacement, 1)
        implementation = output / f"{name}.c"
        implementation.write_text(text)
        test = root / f"tests/test_{'chat_history_job' if kind == 'worker' else 'store_connections'}.c"
        sources = [str(implementation), str(test)]
        if kind == "worker":
            generated = output / f"{name}_test.c"
            generated.write_text(test.read_text().replace('../src/app/chat_history_job.c', str(implementation)))
            sources = [str(generated)]
        binary = output / name
        with (output / f"{name}.log").open("w") as log:
            subprocess.run([*compiler, "-std=c11", "-O1", "-g", "-Wno-deprecated-declarations",
                            "-Isrc", "-Isrc/app", *sources, "-lpthread", "-o", str(binary)],
                           cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
            result = subprocess.run([str(binary)], cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        if replacement is None:
            if result.returncode:
                raise RuntimeError(f"Unmodified control failed: {name}")
            print(f"PASS {name}", flush=True)
        else:
            if not result.returncode:
                raise RuntimeError(f"Mutation survived: {name}")
            print(f"KILLED {name}", flush=True)


if __name__ == "__main__":
    main()
