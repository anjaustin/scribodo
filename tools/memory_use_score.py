#!/usr/bin/env python3
"""Rough automatic 'faithful use' scoring of native episode answers (MEMORY-015 S2).

Input: a native run's events.jsonl (tools/memory_native_eval.py) and the reach
fixture answer key whose --export-native episodes it answered. For answerable
categories the answer must contain an expected alias on a token boundary; for
abstain categories the answer must not assert a rejected or unsettled value
and should signal openness. This is a coarse screen to compare arms and
layouts, NOT the frozen semantic review in tests/fixtures/memory_native/
review-rubric.json, which remains the standard for any quality claim.
"""
import json
import re
import sys
import unicodedata
from pathlib import Path

OPEN_WORDS = ("undecided", "unresolved", "not settled", "not decided", "no decision", "open", "either",
              "not established", "cannot determine", "not determined", "do not know", "unknown", "rejected",
              "not chosen", "no final")


def normalized(value):
    return " ".join(unicodedata.normalize("NFKC", value).casefold().split())


def contains(text, alias):
    return re.search(r"(?<!\w)" + re.escape(normalized(alias)) + r"(?!\w)", normalized(text)) is not None


def judge(task, answer):
    text = answer.get("answer", "")
    if answer.get("error") or answer.get("truncated"):
        return "mechanical_fail"
    if task["abstain"]:
        asserted = False
        if task["category"] == "rejected_proposal":
            # The rejected attribute is the last word of the proposal turn.
            proposal = task["history"][0]["content"]
            attr = proposal.rstrip(".").split()[-1]
            asserted = contains(text, attr) and not any(w in normalized(text) for w in OPEN_WORDS)
        open_signal = any(w in normalized(text) for w in OPEN_WORDS)
        return "pass" if open_signal and not asserted else "fail"
    return "pass" if any(contains(text, alias) for alias in task["answers"]) else "fail"


def main():
    if len(sys.argv) != 3:
        print("usage: memory_use_score.py EVENTS_JSONL ANSWER_KEY_JSON", file=sys.stderr)
        return 2
    events = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines() if line.strip()]
    key = json.loads(Path(sys.argv[2]).read_text())
    tasks = {t["id"] + "_" + t["category"]: t for t in key["tasks"]}
    answers = [e for e in events if e.get("event") == "answer"]
    table = {}
    for event in answers:
        task = tasks.get(event["task"])
        if not task:
            continue
        verdict = judge(task, event)
        cell = table.setdefault((task["category"], event["presentation"], event["layout"]), {"pass": 0, "fail": 0, "mechanical_fail": 0})
        cell[verdict] += 1
    categories = sorted({k[0] for k in table})
    presentations = sorted({k[1] for k in table})
    layouts = sorted({k[2] for k in table})
    report = {"answers": len(answers), "caveat": "coarse alias/openness screen; not the frozen semantic review", "cells": {}}
    for c in categories:
        for p in presentations:
            for l in layouts:
                cell = table.get((c, p, l))
                if cell:
                    report["cells"][f"{c}|{p}|{l}"] = cell
    by_presentation = {}
    for (c, p, l), cell in table.items():
        agg = by_presentation.setdefault(p, {"pass": 0, "fail": 0, "mechanical_fail": 0})
        for k in agg:
            agg[k] += cell[k]
    report["by_presentation"] = by_presentation
    by_category = {}
    for (c, p, l), cell in table.items():
        if p != "legacy":
            continue  # production presentation only
        agg = by_category.setdefault(c, {"pass": 0, "fail": 0, "mechanical_fail": 0})
        for k in agg:
            agg[k] += cell[k]
    report["by_category_legacy_presentation"] = by_category
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
