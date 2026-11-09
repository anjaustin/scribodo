#!/usr/bin/env python3
"""Compare frozen reach runs using one scorer; report counts, never a product score."""
import argparse
import json
from pathlib import Path

import memory_reach_eval as reach


def measure(directory):
    fixture = json.loads((directory / "answer-key.json").read_text())
    events = [json.loads(line) for line in (directory / "events.jsonl").read_text().splitlines() if line.strip()]
    original = json.loads((directory / "report.json").read_text())
    # Preserve the recorded driver outcome, rather than assuming a complete run.
    driver_ok = original.get("gates", {}).get("driver_complete") is True
    report = reach.summarize(fixture, events, 0 if driver_ok else 1,
                             (directory / "FROZEN.sha256").read_text().strip())
    rows = report["per_task"]
    documents = [r for r in rows if r["category"] == "document_fact"]
    chats = [r for r in rows if r["category"] != "document_fact" and r["reach_admitted"] is not None]
    selections = [e for e in events if e.get("event") == "selection"]
    return {
        "directory": str(directory), "harness_gates": report["gates"],
        "database_cleanup_returncode": original.get("database_cleanup_returncode"),
        "document_exact_key_sentences": {"passed": sum(r["whole_sentence_admitted"] == 1 for r in documents), "total": len(documents)},
        "document_exact_key_sentences_discovered": {"passed": sum(r["whole_sentence_candidates"] == 1 for r in documents), "total": len(documents)},
        "document_answer_aliases": {"passed": sum(r["reach_admitted"] == 1 for r in documents), "total": len(documents)},
        "chat_tasks_all_required_sources": {"passed": sum(r["reach_admitted"] == 1 for r in chats), "total": len(chats)},
        "source_range_violations": sum(r["privacy_violations"] for r in rows),
        "maximum_reference_bytes": max((e["reference_bytes"] for e in selections), default=None),
        "maximum_reference_tokens": max((e["reference_tokens"] for e in selections), default=None),
        "planning_over_500ms": report["overall"]["over_budget"],
        "planning_p95_seconds": report["overall"]["planning_p95"],
        "archive": report["archive"],
        "document_misses": [{"task": r["task"], "exact_sentence": r["whole_sentence_admitted"],
                             "answer_alias": r["reach_admitted"]} for r in documents if r["whole_sentence_admitted"] != 1],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    before = json.loads((args.before / "manifest.json").read_text())
    after = json.loads((args.after / "manifest.json").read_text())
    result = {
        "protocol": "memory-reach-comparison-v1",
        "scorer_sha256": reach.sha(Path(reach.__file__)),
        "comparison_sha256": reach.sha(Path(__file__)),
        "same_input": before["input_sha256"] == after["input_sha256"],
        "same_answer_key": before["answer_key_sha256"] == after["answer_key_sha256"],
        "before": measure(args.before), "after": measure(args.after),
        "scope": "Source reach only; no inference of faithful answers, held-out quality, sustained-use readiness or complete-product status",
        "limits": "Fresh databases have different source IDs; tied discovery scores can change candidate order. Reported timings describe these runs, not an isolated benchmark or SLA."
    }
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")
    valid = result["same_input"] and result["same_answer_key"] and all(
        all(result[arm]["harness_gates"].values()) and result[arm]["database_cleanup_returncode"] == 0
        for arm in ("before", "after"))
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
