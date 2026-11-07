#!/usr/bin/env python3
"""Fixed label-binding controls after the 24-case causal DEV contrast.

Changes diagnostic question wording only; never changes production behavior.
"""
import json
from memory_causal_cases import NEUTRAL, STATES
from memory_bedrock_probe import ROOT, c_literal


def cases():
    persona = c_literal((ROOT / "src/llm/chat.c").read_text(), r"static const char \*k_persona") + "\n\nYou are Scribodo."
    rows = []
    for state, (earlier, last, free, classify, expected) in STATES.items():
        transcript = "Conversation excerpts, chronological:\n" + "\n".join(
            f"{role}: {text}" for role, text in [*earlier, ("author", last)])
        cloak = state in ("proposed", "chosen")
        reversed_question = classify.replace("PROPOSED or CHOSEN", "CHOSEN or PROPOSED") if cloak else classify.replace("PENDING or PROTECTED", "PROTECTED or PENDING")
        definition = ("CHOSEN means the author selected saffron. PROPOSED means saffron is only an unselected option. " if cloak else
                      "PENDING means the author wants the true account chosen before continuing. PROTECTED means the author wants both accounts left plausible without settling them. ")
        boolean = ("Has the author chosen saffron? Answer only YES or NO." if cloak else
                   "According to the author's last statement, must we identify the true account before proceeding with the next scene? Answer only YES or NO.")
        sentence = ("State in one sentence whether the author has chosen saffron." if cloak else
                    "State in one sentence whether the author requires us to settle the true account before continuing, or to leave both accounts plausible.")
        questions = (("reversed_labels", reversed_question), ("defined_labels", definition + classify),
                     ("boolean", boolean), ("sentence", sentence))
        for system_name, system in (("neutral", NEUTRAL), ("creative", persona)):
            for form, question in questions:
                rows.append({"name": f"binding_{state}_{system_name}_{form}", "max_tokens": 512,
                             "messages": [{"role": "system", "content": system},
                                          {"role": "user", "content": transcript + "\n\nCurrent question:\n" + question}],
                             "expected_offline_only": expected,
                             "boolean_expected_offline_only": "YES" if state in ("chosen", "pending") else "NO"})
    assert len(rows) == 32 and len({r["name"] for r in rows}) == 32
    return rows


if __name__ == "__main__":
    print(json.dumps(cases(), ensure_ascii=False, indent=2))
