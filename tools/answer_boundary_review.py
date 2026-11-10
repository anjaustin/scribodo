#!/usr/bin/env python3
"""Mechanical review sheet for a boundary-question live run; no inference.

Joins the fixture, the exported messages, the source units and the live capture.
Fills the mechanical columns (designated passage delivered in memory_sources and
in the final request, finish reason, raw/visible lengths, quotation binding) and
leaves fact_selection, unsupported_additions, contradiction_handling, abstention
and completion as explicit human-review fields with the fixture's expectation
beside the visible reply. Nothing here scores the answer.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answer_citation_binding import classify  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'


def binding(user_content, reply, min_chars=25):
    counts, _ = classify(user_content, reply, min_chars)
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--product', type=Path, required=True, help='product-memory-use/<label> directory')
    parser.add_argument('--capture', type=Path, required=True, help='live capture directory with request-N.json')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    messages = json.loads((args.product / 'messages.json').read_text())
    units = json.loads((args.product / 'units.json').read_text())
    by_text = {}
    for p in sorted(args.capture.glob('retrieval-*.json')):
        i = int(p.stem.split('-')[1])
        by_text.setdefault(json.loads(p.read_text())['query'], i)
    users = [m for m in messages if m['role'] == 'user']
    assistants = {m['seq']: m for m in messages if m['role'] == 'assistant'}
    sheet = []
    for case in fixture['cases']:
        q = [m for m in users if m['content'] == case['question']]
        if len(q) != 1:
            sheet.append({'id': case['id'], 'error': f'question appears {len(q)} times in the exported messages'}); continue
        q = q[0]; reply = assistants.get(q['seq'] + 1)
        idx = by_text.get(case['question'])
        request = json.loads((args.capture / f'request-{idx}.json').read_text()) if idx is not None else None
        outcome = json.loads((args.capture / f'outcome-{idx}.json').read_text()) if idx is not None and (args.capture / f'outcome-{idx}.json').exists() else {}
        raw = (args.capture / f'content-{idx}.txt').read_text() if idx is not None and (args.capture / f'content-{idx}.txt').exists() else None
        user_content = request['messages'][-1]['content'] if request else ''
        delivered = []
        for d in case['designated']:
            hits = [u for u in units if u['document_name'] == d['document'] and d['passage'] in u['text']]
            uid = hits[0]['unit_id'] if len(hits) == 1 else None
            delivered.append({'passage': d['passage'], 'unit_id': uid,
                              'admitted_in_memory_sources': bool(uid) and uid in (reply.get('memory_sources') or ''),
                              'passage_in_final_request': d['passage'] in user_content})
        visible = reply['content'] if reply else ''
        sheet.append({'id': case['id'], 'kind': case['kind'], 'question': case['question'], 'capture_index': idx,
                      'designated': delivered, 'finish': outcome.get('truncated'), 'capture_complete': outcome.get('capture_complete'),
                      'raw_characters': len(raw) if raw is not None else None, 'visible_characters': len(visible),
                      'quotation_binding': binding(user_content, visible) if request else None,
                      'expected': case['expected'], 'visible_reply': visible,
                      'human_review': {f: None for f in fixture['review_fields']}})
    out = {'fixture_sha256': hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), 'product': str(args.product), 'capture': str(args.capture), 'cases': sheet}
    with args.output.open('x') as f:
        json.dump(out, f, ensure_ascii=False, indent=2); f.write('\n')
    for c in sheet:
        if 'error' in c: print(c['id'], 'ERROR', c['error']); continue
        print(f"{c['id']}: delivered={[d['admitted_in_memory_sources'] for d in c['designated']]} in_request={[d['passage_in_final_request'] for d in c['designated']]} truncated={c['finish']} visible={c['visible_characters']} binding={c['quotation_binding']}")


if __name__ == '__main__':
    main()
