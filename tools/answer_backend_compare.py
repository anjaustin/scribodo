#!/usr/bin/env python3
"""Compare same-message replies across runs (e.g. strict CPU pq2 vs Metal pc1).

Reports, per arm name present in both runs: finish reasons, completion tokens,
raw length, longest common raw prefix (characters) and whether raw content is
identical. No quality judgment; cleanup to visible text is a separate step
(answer_manifold_visible.py).
"""
import argparse
import json
from pathlib import Path


def rows(run):
    return {r['arm']: r for r in json.loads((run / 'results.json').read_text())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--a', type=Path, required=True)
    parser.add_argument('--b', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    a, b = rows(args.a), rows(args.b)
    out = []
    for arm in sorted(set(a) & set(b)):
        ra, rb = (a[arm]['message'].get('content') or ''), (b[arm]['message'].get('content') or '')
        common = next((i for i, (x, y) in enumerate(zip(ra, rb)) if x != y), min(len(ra), len(rb)))
        out.append({'arm': arm, 'a': str(args.a), 'b': str(args.b),
                    'finish': [a[arm]['finish_reason'], b[arm]['finish_reason']],
                    'completion_tokens': [a[arm]['usage'].get('completion_tokens'), b[arm]['usage'].get('completion_tokens')],
                    'raw_characters': [len(ra), len(rb)], 'common_raw_prefix_characters': common,
                    'raw_identical': ra == rb, 'wall_seconds': [a[arm].get('wall_seconds'), b[arm].get('wall_seconds')]})
    print(json.dumps(out, indent=2))
    if args.output:
        with args.output.open('x') as f:
            json.dump(out, f, indent=2); f.write('\n')


if __name__ == '__main__':
    main()
