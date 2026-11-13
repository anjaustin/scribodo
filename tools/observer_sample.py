#!/usr/bin/env python3
"""A fixed-seed sample of an observer replay's findings per type, as the sheet for the precision audit by hand.

usage: tools/observer_sample.py SEED PER_TYPE FINDINGS.txt

Prints, per type in alphabetical order, up to PER_TYPE lines drawn without replacement with random.Random(SEED)
from that type's findings in file order, each as `type\tindex\tdoc\tstart\tlen\tsubject\tcontent\tquote` (newlines inside a field shown as spaces; index
is the record's position in the file), and a header with the counts and the parser's line statistics; the same seed over the same file gives the same sheet. Verdicts (right, wrong, empty) are written by
hand beside each line in the journal, never by this tool."""
import collections
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import observer_lines  # noqa: E402


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 1
    seed, per_type, path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    by_type = collections.defaultdict(list)
    rows, stats = observer_lines.read(path)
    for i, r in enumerate(rows):
        by_type[r["type"]].append((i, r))
    rng = random.Random(seed)
    print(f"# seed={seed} per_type={per_type} " + " ".join(f"{t}={len(v)}" for t, v in sorted(by_type.items()))
          + " " + observer_lines.stats_line(stats))
    for t in sorted(by_type):
        rows_t = by_type[t]
        picked = sorted(rng.sample(rows_t, min(per_type, len(rows_t))), key=lambda r: r[0])
        for i, r in picked:
            print("\t".join([t, str(i), r["doc"], str(r["start"]), str(r["len"]), r["subject"].replace("\n", " "),
                             r["content"].replace("\n", " "), r["quote"].replace("\n", " ")]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
