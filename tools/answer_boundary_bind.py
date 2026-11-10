#!/usr/bin/env python3
"""Bind the boundary-question fixture to one manuscript unit per passage; no inference.

Refuses any passage that binds to zero or several units, so a later run cannot
silently use a different original than the one declared before testing.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'


def bind(fixture, units):
    bound = []
    for case in fixture['cases']:
        designated = []
        for d in case['designated']:
            hits = [u for u in units if u['document_name'] == d['document'] and d['passage'] in u['text']]
            if len(hits) != 1:
                raise ValueError(f"{case['id']}: passage binds to {len(hits)} units, expected 1: {d['passage']!r}")
            u = hits[0]
            designated.append({**d, 'unit_id': u['unit_id'], 'text_hash': u['text_hash'], 'revision_id': u['revision_id'],
                               'byte_start': u['byte_start'], 'text_bytes': len(u['text'].encode())})
        bound.append({**case, 'designated': designated})
    return bound


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    units = json.loads((args.product / 'units.json').read_text())
    bound = bind(fixture, units)
    with args.output.open('x') as f:
        json.dump({'fixture_sha256': hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
                   'units_sha256': hashlib.sha256((args.product / 'units.json').read_bytes()).hexdigest(),
                   'review_fields': fixture['review_fields'], 'cases': bound}, f, ensure_ascii=False, indent=2)
        f.write('\n')
    print(json.dumps([{'id': c['id'], 'kind': c['kind'], 'designated_units': [d['unit_id'][:12] for d in c['designated']]} for c in bound], indent=2))


if __name__ == '__main__':
    main()
