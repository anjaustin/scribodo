#!/usr/bin/env python3
"""Score an observer replay's findings against the planted faults (recall by fault type) and count the rest by type.

usage: tools/observer_score.py DST_DIR/faults-applied.json FINDINGS.txt [UNPLANTED_FINDINGS.txt]

A planted fault is found when a `conflict` finding's source is the fault's file and its span (start, len) overlaps
the inserted sentence; the same finding counts for one fault only. Prints recall by fault type and in all, the found
and missed ids, and the finding counts by type; with the unplanted run's findings, also how many findings the planted
copy added beyond them. The line format is observer_replay's:
type|subject|relation|object|status|content|source kind|doc or chat|start|len|quote|related text."""
import collections
import json
import sys


def read(path):
    rows = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        parts = line.split("|")
        if len(parts) < 10:
            continue
        rows.append({"type": parts[0], "kind": parts[6], "doc": parts[7], "start": int(parts[8] or 0), "len": int(parts[9] or 0),
                     "quote": parts[10] if len(parts) > 10 else "", "line": line})
    return rows


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 1
    faults = json.load(open(sys.argv[1], encoding="utf-8"))["faults"]
    rows = read(sys.argv[2])
    conflicts = [r for r in rows if r["type"] == "conflict" and r["kind"] == "doc"]
    used = set()
    found = {}
    for f in faults:
        lo, hi = f["offset"], f["offset"] + f["length"]
        for i, r in enumerate(conflicts):
            if i in used or r["doc"] != f["file"]:
                continue
            if r["start"] < hi and r["start"] + r["len"] > lo:
                used.add(i)
                found[f["id"]] = r["line"]
                break
    by_type = collections.OrderedDict()
    for f in faults:
        t = by_type.setdefault(f["type"], [0, 0])
        t[1] += 1
        t[0] += f["id"] in found
    print(f"RECALL planted={len(faults)} found={len(found)} " + " ".join(f"{t}={a}/{b}" for t, (a, b) in by_type.items()))
    print("found: " + " ".join(sorted(found, key=lambda s: (len(s), s))))
    print("missed: " + " ".join(sorted((f["id"] for f in faults if f["id"] not in found), key=lambda s: (len(s), s))))
    for fid, line in sorted(found.items()):
        print(f"  {fid}: {line[:200]}")
    counts = collections.Counter(r["type"] for r in rows)
    print("FINDINGS " + " ".join(f"{t}={n}" for t, n in sorted(counts.items())) + f" total={len(rows)}")
    unmatched = [r for i, r in enumerate(conflicts) if i not in used]
    print(f"conflicts not on a planted fault: {len(unmatched)}")
    if len(sys.argv) == 4:
        base = collections.Counter(r["type"] for r in read(sys.argv[3]))
        print("ADDED over the unplanted run: " + " ".join(f"{t}={counts[t] - base.get(t, 0)}" for t in sorted(set(counts) | set(base))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
