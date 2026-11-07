#!/usr/bin/env python3
"""Finite DEV task/persona contrasts; emits synthetic diagnostic input only.

No model, SQL, author files, production prompt edits or automatic quality score.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from memory_bedrock_probe import ROOT, c_literal

NEUTRAL = "You are a helpful assistant."
PROPOSAL = [
    ("author", "We are brainstorming an inlet scene. No final choice for the keeper's cloak color."),
    ("assistant", "One possibility is saffron."),
]
AMBIGUITY = [
    ("author", "There are two accounts of the lantern incident: Tavi says it fell; Rook says someone pushed it."),
    ("assistant", "We could reveal who is lying later."),
]
STATES = {
    "proposed": (PROPOSAL, "Maybe. Keep saffron as an option; I have not chosen a color.",
                 "Which cloak color did I decide on?", "What is saffron's status? Reply only PROPOSED or CHOSEN.",
                 "PROPOSED; no color selected; author owns the choice, not assistant"),
    "chosen": (PROPOSAL, "Yes. Choose saffron for the keeper's cloak in this draft.",
               "Which cloak color did I decide on?", "What is saffron's status? Reply only PROPOSED or CHOSEN.",
               "CHOSEN; author selected saffron"),
    "protected": (AMBIGUITY, "Do not settle who is lying. Both accounts must remain plausible in this draft.",
                  "Where do we stand on resolving the lantern incident?",
                  "Is finding the true account pending work or deliberately preserved ambiguity? Reply only PENDING or PROTECTED.",
                  "PROTECTED; preserve competing accounts; no obligation to settle them"),
    "pending": (AMBIGUITY, "Before we draft the next scene, we must decide which account is true.",
                "Where do we stand on resolving the lantern incident?",
                "Is finding the true account pending work or deliberately preserved ambiguity? Reply only PENDING or PROTECTED.",
                "PENDING; need to choose an account before the next scene; do not invent the decision"),
}


def cases(source):
    persona = c_literal((ROOT / "src/llm/chat.c").read_text(), r"static const char \*k_persona") + "\n\nYou are Scribodo."
    rows = []
    for state, (earlier, last, free, classify, expected) in STATES.items():
        transcript = "Conversation excerpts, chronological:\n" + "\n".join(
            f"{role}: {text}" for role, text in [*earlier, ("author", last)])
        for system_name, system in (("neutral", NEUTRAL), ("creative", persona)):
            for task_name, task in (("recall", free), ("classify", classify)):
                rows.append({"name": f"minimal_{state}_{system_name}_{task_name}",
                             "messages": [{"role": "system", "content": system},
                                          {"role": "user", "content": transcript + "\n\nCurrent question:\n" + task}],
                             "max_tokens": 512, "expected_offline_only": expected})
    original = [r for line in (source / "events.jsonl").read_text().splitlines()
                if (r := json.loads(line)).get("event") == "answer"][26]
    if (original["task"], original["layout"], original["presentation"]) != ("deliberate_ambiguity", "alternating", "episode"):
        raise RuntimeError("Wrong frozen native input")
    native = json.loads(original["request"])["messages"]
    protected = "Do not settle who is lying. Both accounts must remain plausible in this draft."
    pending = "Before we draft the next scene, we must decide which account is true."
    question = "What caused the fire in our current draft?"
    if native[-1]["content"].count(protected) != 1 or not native[-1]["content"].endswith(question):
        raise RuntimeError("Native source/question no longer matches")
    for state in ("protected", "pending"):
        for system_name, system in (("neutral", NEUTRAL), ("creative", persona)):
            for task_name in ("recall", "classify"):
                messages = copy.deepcopy(native)
                messages[0]["content"] = system
                if state == "pending":
                    messages[-1]["content"] = messages[-1]["content"].replace(protected, pending)
                if task_name == "classify":
                    task = "Is finding the fire's true cause pending work or deliberately preserved ambiguity? Reply only PENDING or PROTECTED."
                    messages[-1]["content"] = messages[-1]["content"][:-len(question)] + task
                rows.append({"name": f"native_{state}_{system_name}_{task_name}", "messages": messages,
                             "max_tokens": 512, "expected_offline_only": STATES[state][-1],
                             "source_events_sha256": hashlib.sha256((source / "events.jsonl").read_bytes()).hexdigest()})
    assert len(rows) == 24 and len({r["name"] for r in rows}) == 24
    return rows


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Frozen native DEV run; never a held-out source")
    args = parser.parse_args()
    print(json.dumps(cases(args.source), ensure_ascii=False, indent=2))
