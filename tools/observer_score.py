#!/usr/bin/env python3
"""Score an observer replay's findings against the planted faults (recall by fault type) and count the rest by type.

usage: tools/observer_score.py DST_DIR/faults-applied.json FINDINGS.txt [UNPLANTED_FINDINGS.txt]

A planted fault is found when a `conflict` finding's source is the fault's file and its span (start, len, in
characters like the fault's char_offset and char_length) overlaps the inserted sentence; the same finding counts for
one fault only. Prints recall by fault type and in all, the found
and missed ids, and the finding counts by type; with the unplanted run's findings, also how many findings the planted
copy added beyond them. The line format is observer_replay's:
type|subject|relation|object|status|content|source kind|doc or chat|start|len|quote|related text, read through
observer_lines.py (escaped fields; findings written across lines before the escaping are joined and counted)."""
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import observer_lines  # noqa: E402


def read(path):
    rows, stats = observer_lines.read(path)
    return rows, stats


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 1
    faults = json.load(open(sys.argv[1], encoding="utf-8"))["faults"]
    rows, stats = read(sys.argv[2])
    conflicts = [r for r in rows if r["type"] == "conflict" and r["kind"] == "doc"]
    used = set()
    found = {}
    for f in faults:
        lo, hi = f["char_offset"], f["char_offset"] + f["char_length"]
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
    print(observer_lines.stats_line(stats))
    unmatched = [r for i, r in enumerate(conflicts) if i not in used]
    print(f"conflicts not on a planted fault: {len(unmatched)}")
    if len(sys.argv) == 4:
        base_rows, base_stats = read(sys.argv[3])
        base = collections.Counter(r["type"] for r in base_rows)
        print("ADDED over the unplanted run: " + " ".join(f"{t}={counts[t] - base.get(t, 0)}" for t in sorted(set(counts) | set(base))))
        print("unplanted " + observer_lines.stats_line(base_stats))
    return 0


if __name__ == "__main__":
    sys.exit(main())
