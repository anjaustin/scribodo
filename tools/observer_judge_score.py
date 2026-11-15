#!/usr/bin/env python3
"""Score rules over the observer's judge calls offline, against planted faults.

usage: tools/observer_judge_score.py JUDGE.jsonl FAULTS_APPLIED.json [--show RULE]

JUDGE.jsonl is observer_replay --judge-log: one JSON object per judge call (kind, doc, start, len in characters,
canon, statement, replies, conflict). FAULTS_APPLIED.json is observer_plant.py's record (file, char_offset,
char_length, canon, id, type). A call is *on target* when its statement's span overlaps a planted sentence in the
same document and its canon entry is that fault's canon statement; every other call is off target. A rule decides,
per call, whether a conflict is reported; it is scored by the faults it finds (distinct ids with an on-target call
kept), the off-target conflicts it keeps, and the precision of what it keeps. Rules marked "pre" use no model reply
and could run before the judge, so the calls they would save are reported too.

The rules are deterministic software over the canon entry, the statement and the judge's reply (MEMORY-015: model
errors are measured; fix-software-not-prompts). They are written on one novel and must hold on another before any
ships (record-model-replies-before-tuning)."""
import collections
import json
import re
import sys

NUMBER_WORDS = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
                "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty",
                "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "thousand", "first", "second",
                "third", "fourth", "fifth", "eldest", "youngest", "twice", "once"}
KIN_WORDS = {"father", "mother", "son", "daughter", "brother", "sister", "aunt", "uncle", "niece", "nephew", "cousin",
             "grandfather", "grandmother", "granddaughter", "grandson", "wife", "husband", "widow", "widower",
             "parent", "parents", "child", "children", "sons", "daughters", "brothers", "sisters", "grandparent"}
COLOR_WORDS = {"black", "white", "red", "blue", "green", "brown", "golden", "gold", "grey", "gray", "yellow",
               "crimson", "scarlet", "silver", "dark", "fair", "pale", "auburn", "copper", "purple", "violet"}
TIME_WORDS = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october",
              "november", "december", "before", "after", "earlier", "later", "years", "year", "days", "day", "months",
              "month", "weeks", "week", "age", "aged", "old"}
TITLES = {"mr", "mrs", "ms", "miss", "lady", "lord", "sir", "dr", "captain", "colonel", "prince", "princess", "king",
          "queen", "the", "a", "an", "he", "she", "it", "they", "his", "her", "canon", "new"}


def words(text):
    """ASCII letters and ASCII digits, as obs_attributes_differ reads them (\\d read Arabic-Indic and other Unicode
    digits until 2026-11-15; tests/fixtures/observer_rules/cases.json, AUDIT-004 F2)."""
    return re.findall(r"[A-Za-z]+|[0-9]+", text or "")


def lower_set(text, vocab):
    return {w.lower() for w in words(text) if w.lower() in vocab}


def numbers(text):
    return {w.lower() for w in words(text) if w.isdigit() or w.lower() in NUMBER_WORDS}


def place_names(text):
    """Capitalised names after of/in/from/at/to: the places and households a sentence puts someone in."""
    return {m.group(2) for m in re.finditer(r"\b(of|in|from|at|to)\s+((?:[A-Z][a-z]+)(?:\s+[A-Z][a-z]+)*)", text or "")
            if m.group(2).split()[0].lower() not in TITLES}


def names(text):
    return {w for w in words(text) if w[0].isupper() and w.lower() not in TITLES and not w.isdigit()}


STOP = set("that this with from have been were when then than they them their there into after before about which "
           "while would could should also only very just more most some such said says will shall upon being other "
           "another whom whose what where here those these over under long last first".split())


def subjects(text):
    """Who a sentence is about: the leading run of capitalised words (the part before an em dash when there is
    one, as in "Kantos Kan \u2014 a padwar ..."), without titles, plus any possessive name ("Sola's father")."""
    text = text or ""
    head = text.split(" \u2014 ")[0] if " \u2014 " in text else text
    run = []
    for w in re.findall(r"[A-Z][a-z]+|[A-Z]\.|[a-z]+", head):
        if not w[0].isupper():
            break
        run.append(w)
    found = {w for w in run if len(w) > 2 and w.lower() not in TITLES}
    return found | {m.group(1) for m in re.finditer(r"\b([A-Z][a-z]+)'s\b", text)}


def slot_words(text):
    """The lower-case content words naming what is said of the subject (padwar, navy, vessel, reached), without
    the attribute values themselves; five-letter prefixes so reached/reaches agree."""
    vocab = KIN_WORDS | COLOR_WORDS | TIME_WORDS | NUMBER_WORDS | STOP
    return {w[:5] for w in re.findall(r"[A-Za-z]+", text or "") if w.islower() and len(w) > 3 and w not in vocab}


def differs(a, b):
    return bool(a) and bool(b) and a != b


def reply_fields(call, p):
    r = (call.get("replies") or [None, None])[p]
    try:
        return json.loads(r) if r else {}
    except ValueError:
        return {}


# A rule takes a call and returns True when a conflict is reported. "pre" rules read no reply.
def r_base(c): return c["conflict"]
def a_num(c): return differs(numbers(c["canon"]), numbers(c["statement"]))
def a_kin(c): return differs(lower_set(c["canon"], KIN_WORDS), lower_set(c["statement"], KIN_WORDS))
def a_color(c): return differs(lower_set(c["canon"], COLOR_WORDS), lower_set(c["statement"], COLOR_WORDS))
def a_time(c): return differs(lower_set(c["canon"], TIME_WORDS) | numbers(c["canon"]), lower_set(c["statement"], TIME_WORDS) | numbers(c["statement"]))
def a_place(c): return differs(place_names(c["canon"]), place_names(c["statement"]))
def a_any(c): return a_num(c) or a_kin(c) or a_color(c) or a_time(c) or a_place(c)
def a_subject(c): return bool(names(c["canon"]) & names(c["statement"]))
def same_subject(c): return bool(subjects(c["canon"]) & subjects(c["statement"]))
def same_slot(c):
    cs = slot_words(c["canon"])
    return not cs or bool(cs & slot_words(c["statement"]))

RULES = collections.OrderedDict([
    ("base: the judge's two votes", (False, r_base)),
    ("judge and a differing number", (False, lambda c: r_base(c) and a_num(c))),
    ("judge and a differing kinship word", (False, lambda c: r_base(c) and a_kin(c))),
    ("judge and a differing colour", (False, lambda c: r_base(c) and a_color(c))),
    ("judge and a differing time word or number", (False, lambda c: r_base(c) and a_time(c))),
    ("judge and a differing place name", (False, lambda c: r_base(c) and a_place(c))),
    ("judge and any differing attribute", (False, lambda c: r_base(c) and a_any(c))),
    ("judge, a shared name, any differing attribute", (False, lambda c: r_base(c) and a_subject(c) and a_any(c))),
    ("judge and the same subject", (False, lambda c: r_base(c) and same_subject(c))),
    ("judge, the same subject, any differing attribute", (False, lambda c: r_base(c) and same_subject(c) and a_any(c))),
    ("judge, the same subject, the same slot", (False, lambda c: r_base(c) and same_subject(c) and same_slot(c))),
    ("pre: a shared name and any differing attribute", (True, lambda c: a_subject(c) and a_any(c))),
    ("pre: the same subject, the same slot, any attribute", (True, lambda c: same_subject(c) and same_slot(c) and a_any(c))),
])


def on_target(call, faults):
    if call.get("kind") != "doc":
        return None
    s, e = call["start"], call["start"] + call["len"]
    for f in faults:
        if f["file"] == call["doc"] and s < f["char_offset"] + f["char_length"] and e > f["char_offset"] \
                and call["canon"].strip() == f["canon"].strip():
            return f
    return None


def score(calls, faults):
    labelled = [(c, on_target(c, faults)) for c in calls]
    reachable = sorted({f["id"] for _, f in labelled if f}, key=lambda s: (len(s), s))
    rows = []
    for name, (pre, rule) in RULES.items():
        kept = [(c, f) for c, f in labelled if rule(c)]
        found = sorted({f["id"] for _, f in kept if f}, key=lambda s: (len(s), s))
        false = sum(1 for _, f in kept if not f)
        prec = (len(kept) - false) / len(kept) if kept else 0.0
        rows.append({"rule": name, "pre": pre, "kept": len(kept), "found": found, "false": false, "precision": prec,
                     "calls_needed": len(kept) if pre else len(calls)})
    return {"calls": len(calls), "faults": len(faults), "reachable": reachable, "rows": rows}


JUDGE_LOG_VERSIONS = (0, 1)  # src/llm/observer_formats.h: 0 is a log from before 2026-11-15, without "version"


def load(path):
    """The judge calls of a log written by observer_replay --judge-log (format: src/llm/observer_formats.h). A line
    whose version this reader does not know is refused, never guessed at."""
    calls = []
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        if not line.strip():
            continue
        call = json.loads(line)
        version = call.get("version", 0)
        if version not in JUDGE_LOG_VERSIONS:
            raise ValueError(f"{path}:{n}: judge log version {version}; this reader knows {JUDGE_LOG_VERSIONS}")
        calls.append(call)
    return calls


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 1
    calls = load(sys.argv[1])
    faults = json.load(open(sys.argv[2], encoding="utf-8"))["faults"]
    res = score(calls, faults)
    print(f"JUDGE calls={res['calls']} faults={res['faults']} reachable={len(res['reachable'])} ({' '.join(res['reachable'])})")
    print(f"{'rule':54} {'kept':>5} {'found':>6} {'false':>6} {'precision':>9}  faults found")
    for r in res["rows"]:
        print(f"{r['rule']:54} {r['kept']:5d} {len(r['found']):6d} {r['false']:6d} {r['precision']:9.2f}  {' '.join(r['found'])}"
              + (f"   (would need {r['calls_needed']} judge calls of {res['calls']})" if r["pre"] else ""))
    if "--show" in sys.argv:
        want = sys.argv[sys.argv.index("--show") + 1]
        rule = next(v[1] for k, v in RULES.items() if k.startswith(want))
        for c in calls:
            if rule(c):
                f = on_target(c, faults)
                print(("TRUE " + f["id"] if f else "FALSE") + f" | {c['doc']} | canon: {c['canon'][:80]} | new: {c['statement'][:120]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
