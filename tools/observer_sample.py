#!/usr/bin/env python3
"""A fixed-seed sample of an observer replay's findings per type, as the sheet for the precision audit by hand.

usage: tools/observer_sample.py SEED PER_TYPE FINDINGS.txt

Prints, per type in alphabetical order, up to PER_TYPE lines drawn without replacement with random.Random(SEED)
from that type's findings in file order, each as `type\tindex\tdoc\tstart\tlen\tsubject\tcontent\tquote`, and a header
with the counts; the same seed over the same file gives the same sheet. Verdicts (right, wrong, empty) are written by
hand beside each line in the journal, never by this tool."""
import collections
import random
import sys


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 1
    seed, per_type, path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    by_type = collections.defaultdict(list)
    for i, line in enumerate(open(path, encoding="utf-8")):
        parts = line.rstrip("\n").split("|")
        if len(parts) < 11 or line.startswith("#"):
            continue
        by_type[parts[0]].append((i, parts))
    rng = random.Random(seed)
    print(f"# seed={seed} per_type={per_type} " + " ".join(f"{t}={len(v)}" for t, v in sorted(by_type.items())))
    for t in sorted(by_type):
        rows = by_type[t]
        picked = sorted(rng.sample(rows, min(per_type, len(rows))), key=lambda r: r[0])
        for i, p in picked:
            print("\t".join([t, str(i), p[7], p[8], p[9], p[1], p[5], p[10]]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
