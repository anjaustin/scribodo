#!/usr/bin/env python3
"""Check bounded planner/control protocols without services or models.

Only generated files in build/memory-pipeline-mutations change. Compilation
failures are errors, not killed mutations; nonzero test assertions kill mutants.
"""
import argparse
import os
import re
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=root / "build/memory-pipeline-mutations",
                        help="where generated sources and logs go (make mutationtest passes its own)")
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    planner = (root / "src/llm/context_planner.c").read_text()
    worker = (root / "src/app/memory_job.c").read_text()
    cases = [
        ("planner_control", "planner", None),
        ("planner_remote", "planner", ("if (remote) return true;", "if (false) return true;")),
        ("planner_database", "planner", ("!strcmp(a->database, b->database) && a->database[0] &&", "a->database[0] &&")),
        ("planner_policy_epoch", "planner", ("a->policy_epoch == b->policy_epoch &&", "")),
        ("planner_tokenizer", "planner", ("if (strcmp(plan.scope.tokenizer, ops->tokenizer))", "if (false)")),
        ("planner_request_integrity", "planner", ("json_parse_complete(base_request, base_bytes)", "json_parse(base_request)")),
        ("planner_final_budget", "planner", ("if ((uint64_t)tokens <= prompt_budget)", "if (true)")),
        ("planner_document_authorization", "planner", ("bool authorized = false;", "bool authorized = true;")),
        ("planner_document_detach", "planner", ("!documents_same(&plan->documents, documents)", "false")),
        ("planner_original_readmission", "planner", ("if (unit->kind != MEMORY_SOURCE_CHAT) continue;", "if (true) continue;")),
        ("planner_fusion_scale", "planner", ("1.0 / (RECALL_FUSION_K + (double)i)", "1.0 / (61.0 + (double)i)")),
        ("worker_control", "worker", None),
        ("worker_pause_controls", "worker", ("if (job->control_count) {", "if (job->control_count && !job->paused) {")),
        ("worker_idle_pacing", "worker", ("if (job->request.serial == serial && !time_due(navigation_due, now))", "if (false)")),
        ("worker_control_backoff", "worker", ("if (!time_due(control_due, now))", "if (false)")),
        ("worker_clear_controls", "worker", ("job->selected = false;\n    atomic_store(&job->generation_cancel, true);\n    job->request = (MemoryRequest){0};", "job->selected = false;\n    atomic_store(&job->generation_cancel, true);\n    job->control_count = 0;\n    job->request = (MemoryRequest){0};")),
        ("worker_cold_tokenizer", "worker", ("if (job->tokenizer.server && llm_server_state(job->tokenizer.server, NULL, 0) != LLM_READY)", "if (false)")),
    ]
    cc = shlex.split(os.environ.get("CC", "cc"))
    for name, kind, replacement in cases:
        text = planner if kind == "planner" else worker
        if replacement:
            if replacement[0] not in text:
                raise RuntimeError(f"Mutation no longer matches: {name}")
            text = text.replace(*replacement, 1)
        implementation = output / f"{name}.c"
        implementation.write_text(text)
        if kind == "planner":
            # The Makefile's own source list for test_context_planner, the planner swapped for the mutant: a
            # hand-kept copy here went stale when recall_terms.c and experiments.c joined it.
            listed = re.search(r"^CONTEXT_PLANNER_TEST_SRCS := (.*)$", (root / "Makefile").read_text(), re.M)
            sources = [str(implementation) if s == "src/llm/context_planner.c" else s for s in listed.group(1).split()]
        else:
            test = output / f"{name}_test.c"
            original = (root / "tests/test_memory_job.c").read_text()
            test.write_text(original.replace("../src/app/memory_job.c", str(implementation)))
            sources = [str(test)]
        binary = output / name
        with (output / f"{name}.log").open("w") as log:
            subprocess.run([*cc, "-std=c11", "-O1", "-g", "-Isrc", "-Isrc/llm", "-Ithird_party/cJSON",
                            *sources, "-lpthread", "-o", str(binary)], cwd=root, stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=60)
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
