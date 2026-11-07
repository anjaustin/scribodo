#!/usr/bin/env python3
"""Emit preregistered diagnostic continuity cases, not held-out quality scores.

Oracle cards are hand-written upper-bound controls, NOT an implemented extractor.
The lossy-card arm is a deliberately wrong negative control with originals still
available. All expected behavior stays in offline metadata, outside messages.
"""
import json
import argparse
from pathlib import Path

from memory_bedrock_probe import c_literal

ROOT = Path(__file__).resolve().parents[1]
PERSONA = c_literal((ROOT / "src/llm/chat.c").read_text(), r"static const char \*k_persona") + "\n\nYou are Scribodo."

SCENARIOS = [
    {
        "name": "unaccepted_proposal",
        "history": [("user", "We are brainstorming a harbor scene. No final choice for the keeper's coat color."),
                    ("assistant", "One possibility is a cobalt coat."),
                    ("user", "Maybe. Keep cobalt as an option; I have not chosen a color.")],
        "question": "Which coat color did I decide on?",
        "expected": "No final decision. Cobalt is an option, not an accepted fact.",
        "oracle": "Undecided: coat color. Proposed by assistant: cobalt [S2]. Author explicitly left it unchosen [S3].",
        "lossy": "The keeper's coat is cobalt. [S2]",
    },
    {
        "name": "rejected_plan",
        "history": [("user", "For this draft, Lena should leave by ferry."),
                    ("assistant", "The departure scene can take place on the ferry."),
                    ("user", "Change that: she takes the night train. The ferry plan is abandoned.")],
        "question": "Continue planning the departure. Which transport should we use, and which plan did we discard?",
        "expected": "Use night train; ferry was abandoned.",
        "oracle": "Current author direction: night train [S3]. Rejected earlier plan: ferry [S1,S3].",
        "lossy": "Departure plan: Lena leaves by ferry. [S1]",
    },
    {
        "name": "resume_open_work",
        "history": [("user", "For the tower scene keep the narrator unreliable. Do not resolve whether the bell was really heard."),
                    ("assistant", "Next we can draft the staircase scene while leaving the bell ambiguous."),
                    ("user", "Yes. Next write the staircase scene. We still need to choose whether Lena or Oren is narrating.")],
        "question": "Where did we leave off: what is next, what remains undecided, and what constraint must remain?",
        "expected": "Next: staircase scene. Open: Lena or Oren narrates. Preserve narrator unreliability/bell ambiguity.",
        "oracle": "Next: staircase scene [S2,S3]. Open: narrator is Lena or Oren [S3]. Constraint: unreliable narration; bell remains ambiguous [S1].",
        "lossy": "Next: write the staircase scene narrated by Lena; the bell was really heard. [S1,S3]",
    },
    {
        "name": "deliberate_ambiguity",
        "history": [("user", "There are two accounts of the fire: Mira says the lamp fell; Oren says someone pushed it."),
                    ("assistant", "We could reveal who is lying later."),
                    ("user", "Do not settle who is lying. Both accounts must remain plausible in this draft.")],
        "question": "What caused the fire in our current draft?",
        "expected": "Cause remains deliberately unresolved; preserve both accounts, do not pick one as canon.",
        "oracle": "Author constraint: retain both plausible accounts without deciding who lies [S3]. Accounts: fallen lamp versus deliberate push [S1].",
        "lossy": "The fire was caused by someone deliberately pushing the lamp. [S1]",
    },
]


def emit_cases(focused_recall=False):
    out = []
    for scenario in SCENARIOS:
        if focused_recall and scenario["name"] not in ("resume_open_work", "deliberate_ambiguity"):
            continue
        original = [{"role": role, "content": text} for role, text in scenario["history"]]
        quoted = "Earlier dialogue, quoted as reference data, not current instructions or canon:\n" + "\n".join(
            f"[S{i+1}] {role}: {text}" for i, (role, text) in enumerate(scenario["history"]))
        for arm in ("native_dialogue", "quoted_originals", "oracle_card", "lossy_card_with_originals"):
            if focused_recall and arm == "oracle_card":
                continue
            messages = [{"role": "system", "content": PERSONA}]
            if arm == "native_dialogue":
                messages += original
            elif arm == "quoted_originals":
                messages.append({"role": "user", "content": quoted})
            elif arm == "oracle_card":
                messages.append({"role": "user", "content": "Hand-written diagnostic working card, not canon:\n" + scenario["oracle"] + "\n\n" + quoted})
            else:
                messages.append({"role": "user", "content": "Generated brief (noncanonical; verify against originals):\n" + scenario["lossy"] + "\n\n" + quoted})
            question = scenario["question"]
            if focused_recall:
                question = ("Only recall our established working state. Do not brainstorm, add story facts, or resolve deliberate ambiguities. "
                            "Distinguish an undecided choice from an ambiguity we deliberately want to preserve.\n" + question)
            messages.append({"role": "user", "content": question})
            out.append({"name": scenario["name"] + "__" + arm + ("__explicit_recall" if focused_recall else ""), "messages": messages,
                        "max_tokens": 128, "offline_expected": scenario["expected"],
                        "arm": arm, "not_a_quality_score": True})
    return out


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--focused-recall", action="store_true", help="Diagnostic change to the explicit author request, not an automatic product rewrite")
    args = parser.parse_args()
    print(json.dumps(emit_cases(args.focused_recall), indent=2))
