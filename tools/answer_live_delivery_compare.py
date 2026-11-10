#!/usr/bin/env python3
"""Compare source delivery between two live boundary runs, field by field; no inference.

Each run is a product-memory-use directory (messages.json, units.json) plus its
live capture (request-N.json, retrieval-N.json). For every designated passage of
the boundary fixture the sheet records, per run: the unit's content hash, its
rank in the captured semantic and lexical candidate lists, whether it was
admitted into the reply's memory_sources, and whether the passage text reached
the final request. Units are matched across runs by content hash, since unit
ids are per import. Replies are recorded for reading, not scored.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'


def load_run(product, capture):
    messages = json.loads((product / 'messages.json').read_text())
    units = json.loads((product / 'units.json').read_text())
    traces = {}
    for p in sorted(capture.glob('retrieval-*.json')):
        i = int(p.stem.split('-')[1])
        trace = json.loads(p.read_text())
        request = json.loads((capture / f'request-{i}.json').read_text())
        traces.setdefault(trace['query'], {'index': i, 'trace': trace, 'request': request})
    return {'messages': messages, 'units': units, 'traces': traces}


def rank_of(candidates, text_hash):
    for position, candidate in enumerate(candidates or [], 1):
        if candidate.get('text_hash') == text_hash:
            return position
    return None


def delivery(run, case):
    users = [m for m in run['messages'] if m['role'] == 'user' and m['content'] == case['question']]
    assistants = {m['seq']: m for m in run['messages'] if m['role'] == 'assistant'}
    if len(users) != 1:
        return {'error': f'question appears {len(users)} times'}
    reply = assistants.get(users[0]['seq'] + 1) or {}
    captured = run['traces'].get(case['question'])
    request_text = captured['request']['messages'][-1]['content'] if captured else ''
    trace = captured['trace'] if captured else {}
    rows = []
    for d in case['designated']:
        hits = [u for u in run['units'] if u['document_name'] == d['document'] and d['passage'] in u['text']]
        if len(hits) != 1:
            rows.append({'passage': d['passage'], 'error': f'binds to {len(hits)} units'}); continue
        unit = hits[0]
        rows.append({'passage': d['passage'], 'text_hash': unit['text_hash'],
                     'semantic_rank': rank_of(trace.get('semantic'), unit['text_hash']),
                     'semantic_candidates': len(trace.get('semantic') or []),
                     'lexical_rank': rank_of(trace.get('lexical'), unit['text_hash']),
                     'admitted': unit['unit_id'] in (reply.get('memory_sources') or ''),
                     'in_final_request': d['passage'] in request_text})
    return {'capture_index': captured['index'] if captured else None, 'semantic_ok': trace.get('semantic_ok'),
            'lexical_ok': trace.get('lexical_ok'), 'designated': rows,
            'reply_sha256': hashlib.sha256((reply.get('content') or '').encode()).hexdigest(),
            'reply_characters': len(reply.get('content') or ''), 'truncated': reply.get('truncated')}


def compare(fixture, runs):
    sheet = []
    for case in fixture['cases']:
        row = {'id': case['id'], 'kind': case['kind']}
        for name, run in runs.items():
            row[name] = delivery(run, case)
        sheet.append(row)
    names = list(runs)
    totals = {}
    for name in names:
        rows = [d for c in sheet for d in c[name].get('designated', []) if 'error' not in d]
        totals[name] = {'designated': len(rows), 'admitted': sum(d['admitted'] for d in rows),
                        'in_final_request': sum(d['in_final_request'] for d in rows),
                        'semantic_candidate': sum(d['semantic_rank'] is not None for d in rows),
                        'lexical_candidate': sum(d['lexical_rank'] is not None for d in rows)}
    if len(names) == 2:
        a, b = names
        totals['replies_identical'] = sum(c[a].get('reply_sha256') == c[b].get('reply_sha256') for c in sheet)
    return {'fixture_sha256': hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), 'runs': names, 'totals': totals, 'cases': sheet}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='append', nargs=3, metavar=('NAME', 'PRODUCT', 'CAPTURE'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    runs = {name: load_run(Path(product), Path(capture)) for name, product, capture in args.run}
    out = compare(fixture, runs)
    with args.output.open('x') as f:
        json.dump(out, f, ensure_ascii=False, indent=2); f.write('\n')
    print(json.dumps(out['totals'], indent=2))
    for c in out['cases']:
        for name in out['runs']:
            d = c[name]
            if 'error' in d:
                print(f"{c['id']:28s} {name:10s} ERROR {d['error']}"); continue
            print(f"{c['id']:28s} {name:10s} " + '  '.join(
                f"sem={x.get('semantic_rank')}/{x.get('semantic_candidates')} lex={x.get('lexical_rank')} admitted={x.get('admitted')} in_request={x.get('in_final_request')}"
                for x in d['designated']) + f"  reply_chars={d['reply_characters']}")


if __name__ == '__main__':
    main()
