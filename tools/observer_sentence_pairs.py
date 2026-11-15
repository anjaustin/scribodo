#!/usr/bin/env python3
"""Pair a planted novel's source sentences with canon entries by the observer's own score, offline (GAP-001).

usage: tools/observer_sentence_pairs.py PLANTED_DIR [--sample N SEED OUT.jsonl] [--min M] [--top K]

PLANTED_DIR is observer_plant.py's output (its .md files and faults-applied.json, whose distinct canon statements are
the canon). Every sentence of every file is scored against every canon entry with obs_overlap (build/observer_overlap,
the product's function). For each rule "score at least M, the K best entries of a sentence" the grid prints the judge
pairs it makes and the planted faults it *reaches* (the fault's own sentence paired with the fault's own canon).
--sample writes, for build/observer_judge_pairs, every fault's own pair (label "fault", whatever its score) and N
pairs drawn with SEED from the sentences on no fault under --min/--top (label "plain"; the novels' own sentences, so
a conflict on one is false).

Sentences: each line is split after . ! ? and any closing quotes when a space or tab and a capital (or an opening quote
and a capital) follow, except after a title or an initial (Mr., K.; observer_plant.ends_sentence): the observer's own
obs_next_sentence, held to it by tests/fixtures/observer_rules/cases.json."""
import json
import os
import random
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from observer_plant import ends_sentence  # noqa: E402

BOUNDARY = re.compile("[.!?][\"'”’)]*[ \t]+(?=[\"“‘'(]?[A-Z])")
SPACE = " \t\r"  # what obs_next_sentence skips before a sentence and drops after one (with the line break)


def sentences(text):
    """(character offset, sentence) for every sentence of text, as the observer's obs_next_sentence cuts them: a line
    break ends a sentence; only a space or a tab separates two (a no-break space does not); leading spaces belong to
    no sentence. tests/fixtures/observer_rules/cases.json holds this and the C to the same sentences (AUDIT-004 F2;
    until 2026-11-15 this split at any Unicode space and kept a line's leading spaces)."""
    out = []
    for para in re.finditer(r"[^\n]+", text):
        p, base = para.group(), para.start()
        s = len(p) - len(p.lstrip(SPACE))
        for m in BOUNDARY.finditer(p, s):
            cut = len(p[:m.end()].rstrip(SPACE))
            if not ends_sentence(p[s:cut], p[cut:cut + 1]):
                continue
            out.append((base + s, p[s:cut]))
            s = m.end()
        if p[s:].strip(SPACE):
            out.append((base + s, p[s:].rstrip(SPACE)))
    return out


def load(planted):
    faults = json.load(open(os.path.join(planted, "faults-applied.json"), encoding="utf-8"))["faults"]
    canon = list(dict.fromkeys(f["canon"] for f in faults))
    rows = [(fn, off, s) for fn in sorted(os.listdir(planted)) if fn.endswith(".md")
            for off, s in sentences(open(os.path.join(planted, fn), encoding="utf-8").read())]
    inp = "".join(f"{c}\t{s.replace(chr(9), ' ')}\n" for _, _, s in rows for c in canon)
    out = subprocess.run([os.path.join(ROOT, "build/observer_overlap")], input=inp, capture_output=True, text=True,
                         check=True).stdout.split()
    score = {(r, c): int(out[i * len(canon) + j]) for i, r in enumerate(rows) for j, c in enumerate(canon)}
    own = {}
    for f in faults:
        r = next(r for r in rows if r[0] == f["file"] and r[1] <= f["char_offset"] < r[1] + len(r[2]))
        own[f["id"]] = (r, f["canon"])
    return faults, canon, rows, score, own


def kept(r, canon, score, m, k):
    return [c for c in sorted(canon, key=lambda c: -score[(r, c)])[:k] if score[(r, c)] >= m]


def grid(canon, rows, score, own, mins=(2, 3, 4, 5, 6), tops=(1, 2, 3)):
    res = []
    for m in mins:
        for k in tops:
            pairs = sum(len(kept(r, canon, score, m, k)) for r in rows)
            reached = [i for i, (r, c) in own.items() if c in kept(r, canon, score, m, k)]
            res.append((m, k, pairs, sorted(reached, key=lambda x: (len(x), x))))
    return res


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 1
    a = sys.argv
    m = int(a[a.index("--min") + 1]) if "--min" in a else 3
    k = int(a[a.index("--top") + 1]) if "--top" in a else 2
    faults, canon, rows, score, own = load(a[1])
    print(f"SENTENCES {len(rows)} canon={len(canon)} faults={len(faults)}")
    print("own scores: " + " ".join(f"{i}={score[own[i]]}" for i in sorted(own, key=lambda x: (len(x), x))))
    for mm, kk, pairs, reached in grid(canon, rows, score, own):
        print(f"score>={mm} top{kk}: pairs={pairs:6d} reached={len(reached):2d}/{len(own)} {' '.join(reached)}")
    if "--sample" in a:
        i = a.index("--sample")
        n, seed, path = int(a[i + 1]), int(a[i + 2]), a[i + 3]
        novel = os.path.basename(os.path.normpath(a[1]))
        on_fault = {r for r, _ in own.values()}
        pool = [(r, c) for r in rows if r not in on_fault for c in kept(r, canon, score, m, k)]
        random.Random(seed).shuffle(pool)
        with open(path, "a", encoding="utf-8") as out:
            for f in faults:
                r, c = own[f["id"]]
                out.write(json.dumps(dict(novel=novel, label="fault", id=f["id"], doc=r[0], start=r[1], score=score[(r, c)],
                                          canon=c, statement=r[2]), ensure_ascii=False) + "\n")
            for r, c in pool[:n]:
                out.write(json.dumps(dict(novel=novel, label="plain", id="", doc=r[0], start=r[1], score=score[(r, c)],
                                          canon=c, statement=r[2]), ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
