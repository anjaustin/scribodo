#!/usr/bin/env python3
"""Freeze the explicit Recall/Draft DEV comparison and run native SQL inference.

Build first: bash tools/memory_native_eval.sh --build-only
No production routing changes, automatic classifier, remote provider, or T1.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1048576), b""):
            h.update(chunk)
    return h.hexdigest()


def cases(registration):
    original = json.loads((ROOT / "tests/fixtures/memory_native/dev.json").read_text())["cases"]
    result = copy.deepcopy(original)
    for case in result:
        case["operation"] = "draft" if case["name"] == "current_override" else "recall"
    indexed = {case["name"]: case for case in original}
    accepted = copy.deepcopy(indexed["unaccepted_proposal"])
    accepted.update(name="accepted_proposal", operation="recall",
                    expected="The author chose cobalt for this draft; correctly distinguish the accepted counterfactual from the merely proposed case.")
    accepted["history"][-1]["content"] = "I have decided on cobalt for the keeper's coat in this draft."
    result.append(accepted)
    for name, source, question, expected in (
        ("draft_proposed", "unaccepted_proposal",
         "Draft two sentences for the harbor scene. Keep the keeper's coat color unspecified; cobalt is still only an option.",
         "Two scene sentences; coat color unspecified, cobalt not silently selected; novel prose allowed, not claimed as prior decisions."),
        ("draft_ambiguity", "deliberate_ambiguity",
         "Draft two sentences about the fire, keeping both Mira's fallen-lamp account and Oren's pushed-lamp account plausible. Do not reveal who is right.",
         "Two draft sentences; retain both accounts without choosing the true cause, culprit, or a promised later resolution. Novel scene detail allowed, not asserted as prior canon.")
    ):
        result.append({"name": name, "history": copy.deepcopy(indexed[source]["history"]),
                       "question": question, "expected": expected, "operation": "draft"})
    assert len(result) == 10 and sum(case["operation"] == "recall" for case in result) == 7
    return {"protocol": registration["protocol"], "personas": registration["personas"], "cases": result}


def request_preserved(row, questions):
    try:
        messages = json.loads(row["request"])["messages"]
        current = messages[-1]
        return current["role"] == "user" and current["content"].endswith(questions[row["task"]])
    except (KeyError, IndexError, TypeError, ValueError):
        return False


def main():
    fixture = ROOT / "tests/fixtures/memory_native"
    registration = json.loads((fixture / "operations-registration.json").read_text())
    run = Path(tempfile.mkdtemp(prefix="mo-", dir=ROOT / "build"))
    run.chmod(0o700)
    input_data = cases(registration)
    (run / "cases.json").write_text(json.dumps(input_data, ensure_ascii=False, indent=2))
    shutil.copyfile(fixture / "operations-registration.json", run / "preregistration.json")
    binary = run / "native-eval"
    shutil.copyfile(ROOT / "build/memory_native_eval", binary)
    binary.chmod(0o700)
    for name in ("memory_operation_eval.py", "memory_native_eval.c", "memory_native_eval.sh"):
        shutil.copyfile(ROOT / "tools" / name, run / name)
    database = f"scribodo_test_operation_{os.getpid()}_{secrets.token_hex(4)}"
    manifest = {"protocol": registration["protocol"], "split": "development", "database": database,
                "expected_answers": registration["expected_answers"], "binary_sha256": sha(binary),
                "cases_sha256": sha(run / "cases.json"), "registration_sha256": sha(run / "preregistration.json"),
                "source_fixture_sha256": sha(fixture / "dev.json"),
                "tools_sha256": {name: sha(run / name) for name in ("memory_operation_eval.py", "memory_native_eval.c", "memory_native_eval.sh")},
                "production_source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sorted((ROOT / "src").rglob("*")) if p.suffix in (".c", ".h")}}
    (run / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"Frozen native operation DEV run: {run}", flush=True)
    subprocess.run(["tools/services.sh", "testdb", "create", database], cwd=ROOT, check=True)
    env = os.environ.copy()
    env.setdefault("SCRIBODO_LLAMA_SERVER", str(ROOT / "build/llama/bin/llama-server"))
    process = None
    code = -1
    started = time.monotonic()
    try:
        with (run / "driver.log").open("w") as log:
            process = subprocess.Popen([str(binary), str(run / "cases.json"), str(run), database, str(run / "events.jsonl")],
                                       cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            code = process.wait(timeout=3600)
    finally:
        if process:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            if process.poll() is None:
                process.wait(timeout=20)
        subprocess.run(["tools/services.sh", "testdb", "drop", database], cwd=ROOT, check=True)
    rows = [json.loads(line) for line in (run / "events.jsonl").read_text().splitlines()]
    answers = [row for row in rows if row.get("event") == "answer"]
    cohorts = {}
    questions = {case["name"]: case["question"] for case in input_data["cases"]}
    for row in answers:
        cohorts.setdefault((row["task"], row["layout"]), []).append(row)

    def ids(row):
        return set(re.findall(r"unit=([0-9a-f]{64})", row["sources"]))

    mechanics = {
        "driver_complete": code == 0 and len(answers) == registration["expected_answers"],
        "sources_present": bool(answers) and all(row["sources"] for row in answers),
        "same_source_ids": len(cohorts) == 20 and all(len(group) == 2 and
            {r["arm"] for r in group} == {"generalist", "specialized"} and
            len(ids(group[0])) == 3 and ids(group[0]) == ids(group[1]) for group in cohorts.values()),
        "current_requests_preserved": bool(answers) and all(request_preserved(row, questions) for row in answers),
        "no_errors_or_truncation": bool(answers) and all(not row["error"] and not row["truncated"] for row in answers),
        "cases_intact": sha(run / "cases.json") == manifest["cases_sha256"],
        "registration_intact": sha(run / "preregistration.json") == manifest["registration_sha256"]}
    report = {"result": "SEMANTIC_REVIEW_REQUIRED" if all(mechanics.values()) else "MECHANICAL_GATE_FAILED",
              "quality_pass": False, "answers": len(answers), "runtime_seconds": time.monotonic() - started,
              "mechanics": mechanics, "not_heldout_evidence": True,
              "review": "Review each complete response against frozen Recall/Draft clauses. Valid source admission does not establish semantic fidelity."}
    (run / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)
    return 0 if all(mechanics.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
