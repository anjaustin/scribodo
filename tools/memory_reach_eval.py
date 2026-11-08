#!/usr/bin/env python3
"""Reach measurement (MEMORY-015 S2): did the right source reach the window?

Builds the realistic fixture (tools/memory_fixture.py), freezes it with the
driver and scorer, ingests it through production ingestion and indexing, runs
the production planner and admission for every task with NO generation, and
scores per category:

  reach_candidates   required turns among the 32 lexical+semantic candidates
  reach_admitted     required turns among the admitted (<= 8) excerpts
  lexical_hit / semantic_hit   which discovery path reached them
  distractors_admitted         planted near-misses that were admitted
  planning_seconds             against the production 500 ms budget

Reach is a measurement, not a pass/fail gate. The harness gates only its own
validity: driver complete, every task once, both discovery paths available,
ingestion and indexing complete, zero privacy violations, and the control
category reaching 1.0. The model's use of admitted sources is measured
separately by tools/memory_native_eval.py on --export-native episodes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import memory_fixture  # noqa: E402

CANARY = "PRIVATE-OTHER-CONVERSATION-5d2e7a90 Zoë Íñigo Kestrel archive key birthday 777771"
BUDGET_SECONDS = 0.5


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(q * (len(ordered) - 1))))
    return ordered[index]


def score_task(task, event, archive):
    required = set(task["required_sequences"])
    distractors = set(task["distractor_sequences"])
    lexical = {c["sequence"] for c in event.get("lexical_candidates", []) if isinstance(c.get("sequence"), int)}
    semantic = {c["sequence"] for c in event.get("semantic_candidates", []) if isinstance(c.get("sequence"), int)}
    candidates = lexical | semantic
    admitted_rows = event.get("admitted_units", [])
    admitted = set()
    privacy = 0
    for row in admitted_rows:
        sequence = row.get("sequence")
        text = row.get("text", "")
        if (not isinstance(sequence, int) or not 0 <= sequence < len(archive) or not text or
                text not in archive[sequence]["text"] or CANARY in text):
            privacy += 1
            continue
        admitted.add(sequence)
    result = {"task": task["id"], "category": task["category"], "required": sorted(required),
              "admitted": sorted(admitted), "admitted_count": len(admitted),
              "candidate_count": len(candidates), "privacy_violations": privacy,
              "planning_seconds": event.get("planning_seconds"),
              "over_budget": (event.get("planning_seconds") or 0) > BUDGET_SECONDS,
              "lexical_ok": event.get("lexical_ok") is True, "semantic_ok": event.get("semantic_ok") is True,
              "planned": event.get("planned") is True, "admitted_ok": event.get("admitted") is True,
              "distractors_admitted": len(admitted & distractors), "distractors_planted": len(distractors)}
    if required:
        result["reach_candidates"] = len(required & candidates) / len(required)
        result["reach_admitted"] = len(required & admitted) / len(required)
        result["lexical_hit"] = len(required & lexical) / len(required)
        result["semantic_hit"] = len(required & semantic) / len(required)
    else:
        result["reach_candidates"] = result["reach_admitted"] = result["lexical_hit"] = result["semantic_hit"] = None
    return result


def mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


def summarize(fixture, events, driver_rc, manifest_hash):
    archive = fixture["archive"]
    tasks = {t["id"]: t for t in fixture["tasks"]}
    selections = [e for e in events if e.get("event") == "selection"]
    scored = [score_task(tasks[e["task"]], e, archive) for e in selections if e.get("task") in tasks]
    categories = {}
    for row in scored:
        categories.setdefault(row["category"], []).append(row)
    per_category = {}
    for name, rows in sorted(categories.items()):
        per_category[name] = {
            "n": len(rows),
            "reach_candidates": mean(r["reach_candidates"] for r in rows),
            "reach_admitted": mean(r["reach_admitted"] for r in rows),
            "lexical_hit": mean(r["lexical_hit"] for r in rows),
            "semantic_hit": mean(r["semantic_hit"] for r in rows),
            "mean_admitted": mean(r["admitted_count"] for r in rows),
            "distractors_admitted": sum(r["distractors_admitted"] for r in rows),
            "distractors_planted": sum(r["distractors_planted"] for r in rows),
            "planning_p50": percentile([r["planning_seconds"] for r in rows if r["planning_seconds"] is not None], .5),
            "planning_p95": percentile([r["planning_seconds"] for r in rows if r["planning_seconds"] is not None], .95),
            "over_budget": sum(r["over_budget"] for r in rows)}
    measured = [r for r in scored if r["reach_admitted"] is not None]
    archive_event = next((e for e in events if e.get("event") == "archive"), None)
    complete = [e for e in events if e.get("event") == "complete"]
    cleanup = [e for e in events if e.get("event") == "namespace_cleanup"]
    control = per_category.get("control", {})
    gates = {
        "driver_complete": driver_rc == 0 and bool(complete) and complete[-1].get("driver_ok") is True,
        "all_tasks_once": sorted(r["task"] for r in scored) == sorted(tasks) and len(scored) == len(tasks),
        "all_planned_and_admitted": bool(scored) and all(r["planned"] and r["admitted_ok"] for r in scored),
        "lexical_and_semantic_available": bool(scored) and all(r["lexical_ok"] and r["semantic_ok"] for r in scored),
        "ingestion_and_index_complete": bool(archive_event) and archive_event.get("pending_sequence") == archive_event.get("ingested_sequence")
                                        and archive_event.get("index_pending") is False,
        "privacy_zero": bool(scored) and all(r["privacy_violations"] == 0 for r in scored),
        "control_reach_admitted_1_0": control.get("reach_admitted") == 1.0,
        "namespaces_cleaned": bool(cleanup) and all(c.get("ok") for c in cleanup),
    }
    return {"protocol": "memory-reach-v1", "result": "MEASURED" if all(gates.values()) else "HARNESS_GATE_FAILED",
            "reach_is_a_measurement_not_a_pass": True, "gates": gates, "manifest_sha256": manifest_hash,
            "overall": {"tasks": len(scored), "measured_tasks": len(measured),
                        "reach_candidates": mean(r["reach_candidates"] for r in measured),
                        "reach_admitted": mean(r["reach_admitted"] for r in measured),
                        "lexical_hit": mean(r["lexical_hit"] for r in measured),
                        "semantic_hit": mean(r["semantic_hit"] for r in measured),
                        "over_budget": sum(r["over_budget"] for r in scored),
                        "planning_p50": percentile([r["planning_seconds"] for r in scored if r["planning_seconds"] is not None], .5),
                        "planning_p95": percentile([r["planning_seconds"] for r in scored if r["planning_seconds"] is not None], .95)},
            "per_category": per_category, "per_task": scored, "archive": archive_event,
            "coverage": {"generation": "none; faithful use is measured separately by tools/memory_native_eval.py",
                         "documents": "chat archive only in this protocol; saved-revision reach is a later axis",
                         "selection": "production memory_plan_recall with the production lexical, semantic and hydration callbacks observed, not replaced",
                         "budget": "planning time compared with the production 500 ms memory budget; the planner here is not cancelled at the budget"}}


def self_test():
    for seed in (1, 20261107, 777):
        fixture = memory_fixture.build(seed, 12000, 2)
        by = {}
        for task in fixture["tasks"]:
            by[task["category"]] = by.get(task["category"], 0) + 1
            assert all(s < fixture["boundary"] for s in task["required_sequences"]), task
            if task["category"] not in ("absence",):
                assert task["required_sequences"], task
            if task["category"] == "stated_fact":
                assert len(task["distractor_sequences"]) == 2, task
            if task["category"] == "superseded_fact":
                assert len(task["required_sequences"]) == 2 and task["required_sequences"][0] < task["required_sequences"][1]
        assert by == {c: 2 for c in ("control", "stated_fact", "superseded_fact", "implied_decision", "rejected_proposal",
                                      "protected_ambiguity", "paraphrase", "multilingual", "absence")}, by
        again = memory_fixture.build(seed, 12000, 2)
        assert json.dumps(again, sort_keys=True) == json.dumps(fixture, sort_keys=True), "fixture not deterministic"
        assert fixture["words"] >= 12000
    print("self-test ok: deterministic fixtures, categories, boundaries, leak checks")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seed", type=int, default=20261107)
    parser.add_argument("--words", type=int, default=30000)
    parser.add_argument("--per-category", type=int, default=3)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--export-native", type=Path, help="also write native 'faithful use' episodes here")
    parser.add_argument("--binary", type=Path, default=ROOT / "build/memory-reach/memory_reach_eval")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not 10000 <= args.words <= 300000 or not 1 <= args.per_category <= 12:
        parser.error("words must be 10000..300000; per-category 1..12")
    os.environ.setdefault("SCRIBODO_LLAMA_SERVER", str(ROOT / "build/llama/bin/llama-server"))
    fixture = memory_fixture.build(args.seed, args.words, args.per_category)
    name = f"scribodo_test_reach_{os.getpid()}_{secrets.token_hex(8)}"
    run = ROOT / "build/memory-reach/runs" / name
    run.mkdir(parents=True, mode=0o700)
    os.chmod(run, 0o700)
    (run / "owned-database").write_text(name)
    driver_input = {"protocol": "memory-reach-v1", "archive": fixture["archive"], "boundary": fixture["boundary"],
                    "tasks": [{"id": t["id"], "question": t["question"]} for t in fixture["tasks"]],
                    "private_canary": CANARY}
    save(run / "input.json", driver_input)
    save(run / "answer-key.json", fixture)  # never passed to the driver
    if args.export_native:
        count = memory_fixture.export_native(fixture, args.export_native)
        print(f"native episodes written: {count} -> {args.export_native}")
    sources = [ROOT / "tools/memory_fixture.py", Path(__file__), ROOT / "tools/memory_reach_eval.c", ROOT / "tools/memory_reach_eval.sh"]
    binary = args.binary.resolve()
    owned = run / "memory_reach_eval"
    if binary.is_file():
        shutil.copyfile(binary, owned)
        owned.chmod(0o500)
    for source in sources:
        target = run / "frozen-sources" / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    manifest = {"protocol": "memory-reach-v1", "frozen_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "seed": args.seed, "requested_words": args.words, "actual_words": fixture["words"],
                "turns": len(fixture["archive"]), "boundary": fixture["boundary"], "task_count": len(fixture["tasks"]),
                "per_category": args.per_category, "database": name,
                "input_sha256": sha(run / "input.json"), "answer_key_sha256": sha(run / "answer-key.json"),
                "source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources},
                "binary_sha256": sha(owned) if owned.is_file() else None,
                "git_head": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
                "budget_seconds": BUDGET_SECONDS,
                "configuration": "pinned supervisor defaults; chat server for tokenization only; Qwen embedder; production planner and admission; no generation"}
    save(run / "manifest.json", manifest)
    manifest_hash = sha(run / "manifest.json")
    (run / "FROZEN.sha256").write_text(manifest_hash + "\n")
    print(f"Frozen reach run: {run}\nmanifest_sha256={manifest_hash}", flush=True)
    if args.prepare_only:
        print("Prepared only: NOT RUN.")
        return 0
    if not owned.is_file():
        parser.error("native binary missing; run tools/memory_reach_eval.sh")
    env = dict(os.environ, SCRIBODO_DB_NAME=name)
    env.pop("SCRIBODO_LLM", None)
    services = ROOT / "tools/services.sh"
    started = time.monotonic()
    driver_rc = 1
    cleanup_rc = None
    try:
        subprocess.run([str(services), "testdb", "create", name], cwd=ROOT, env=env, check=True)
        with (run / "driver.stdout.log").open("w") as out, (run / "driver.stderr.log").open("w") as err:
            driver_rc = subprocess.run([str(owned), name, str(run), str(run / "input.json"), str(run / "events.jsonl")],
                                       cwd=ROOT, env=env, stdout=out, stderr=err).returncode
    except (OSError, subprocess.CalledProcessError, KeyboardInterrupt) as error:
        (run / "orchestration-error.txt").write_text(str(error) + "\n")
    finally:
        cleanup_rc = subprocess.run([str(services), "testdb", "drop", name], cwd=ROOT, env=env).returncode
    events = []
    path = run / "events.jsonl"
    if path.exists():
        for line in path.read_text().splitlines():
            try:
                events.append(json.loads(line))
            except ValueError:
                events.append({"event": "malformed", "line": line})
    report = summarize(fixture, events, driver_rc, manifest_hash)
    report["wall_seconds"] = time.monotonic() - started
    report["database_cleanup_returncode"] = cleanup_rc
    if cleanup_rc != 0:
        report["result"] = "HARNESS_GATE_FAILED"
    save(run / "report.json", report)
    print(json.dumps({"result": report["result"], "gates": report["gates"], "overall": report["overall"],
                      "per_category": {k: {kk: v[kk] for kk in ("n", "reach_candidates", "reach_admitted", "lexical_hit",
                                                                "semantic_hit", "distractors_admitted", "planning_p95", "over_budget")}
                                       for k, v in report["per_category"].items()}}, indent=2, ensure_ascii=False))
    return 0 if report["result"] == "MEASURED" else 1


if __name__ == "__main__":
    sys.exit(main())
