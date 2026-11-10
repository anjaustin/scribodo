#!/usr/bin/env python3
"""Review sheet for a verified-pipeline live run; no inference.

Joins the boundary fixture, a run's exported messages and units, and the trace
lines the pipeline writes under SCRIBODO_MEMORY_TRACE=1 (one ANSWER_PIPELINE line
and one ANSWER_PIPELINE_SCORES line per question, in order). For each case:
the chosen unit and whether it is a designated unit, the designated units'
scores and ranks among the scored candidates (or that they were not candidates),
the verified span and status, timings, the composer window, and the visible
reply for reading. Judgment fields are left to the human review.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'
TRACE = re.compile(r'ANSWER_PIPELINE candidates=(\d+) hydrated=(\d+) scored=(\d+) failures=(\d+) top=(\S+) score=([0-9.]+) status=(\w+) '
                   r'rerank_ms=([0-9.]+) extract_ms=([0-9.]+) request_bytes=(\d+)(?: history_first=(\d+))?')
SCORES = re.compile(r'ANSWER_PIPELINE_SCORES((?: [0-9a-f]{12}:[0-9.]+)*)')
RECEIPT = re.compile(r'--- VERIFIED SOURCE \| (?:unit=([0-9a-f]+) \| document=([^|]+) \| score=([0-9.]+) \| status=(\w+)|none \| status=(\w+))')


def parse_trace(log_text):
    """(trace, scores) pairs in question order; scores may be empty for runs without the score line."""
    traces = []
    pending = None
    for line in log_text.splitlines():
        m = TRACE.search(line)
        if m:
            if pending:
                traces.append((pending, []))
            pending = {'candidates': int(m[1]), 'hydrated': int(m[2]), 'scored': int(m[3]), 'failures': int(m[4]), 'top': m[5],
                       'score': float(m[6]), 'status': m[7], 'rerank_ms': float(m[8]), 'extract_ms': float(m[9]),
                       'request_bytes': int(m[10]), 'history_first': int(m[11]) if m[11] else None}
            continue
        s = SCORES.search(line)
        if s and pending:
            scores = [(p.split(':')[0], float(p.split(':')[1])) for p in s[1].split()]
            traces.append((pending, scores))
            pending = None
    if pending:
        traces.append((pending, []))
    return traces


def designated_units(case, units):
    out = []
    for d in case['designated']:
        hits = [u for u in units if u['document_name'] == d['document'] and d['passage'] in u['text']]
        out.append({'passage': d['passage'], 'unit_id': hits[0]['unit_id'] if len(hits) == 1 else None, 'binds': len(hits)})
    return out


def review(fixture, messages, units, traces):
    users = [m for m in messages if m['role'] == 'user']
    assistants = {m['seq']: m for m in messages if m['role'] == 'assistant'}
    # Traces are matched to replies by the chosen unit id (the receipt carries the full id, the trace line its
    # prefix), never by position: a turn answered without the pipeline (for example before memory is enabled)
    # leaves no trace line, and positional pairing would shift every later row (found on pass 2).
    # The same unit can be chosen for two turns, so traces are consumed in reply order: each reply takes the first
    # unconsumed trace whose top matches its receipt.
    by_seq = {}
    unused = list(traces)
    for m in sorted(messages, key=lambda m: m['seq']):
        if m['role'] != 'assistant':
            continue
        r = RECEIPT.search(m.get('memory_sources') or '')
        unit = r[1] if r and r[1] else None
        if not unit:
            continue
        for i, (t, sc) in enumerate(unused):
            if t.get('top', '').startswith(unit[:12]):
                by_seq[m['seq']] = (t, sc); del unused[i]; break

    def trace_for(seq):
        return by_seq.get(seq, ({}, []))
    sheet = []
    for case in fixture['cases']:
        q = [m for m in users if m['content'] == case['question']]
        if len(q) != 1:
            sheet.append({'id': case['id'], 'error': f'question appears {len(q)} times'}); continue
        q = q[0]
        reply = assistants.get(q['seq'] + 1) or {}
        receipt = reply.get('memory_sources') or ''
        m = RECEIPT.search(receipt)
        chosen = {'unit_id': m[1], 'document': m[2].strip(), 'score': float(m[3]), 'status': m[4]} if m and m[1] else \
                 {'unit_id': None, 'status': m[5] if m else None}
        span = receipt.split('---\n', 1)[1].strip() if '---\n' in receipt else ''
        trace, scores = trace_for(q['seq'] + 1)
        ranked = sorted(scores, key=lambda s: -s[1])
        rank_of = {uid: (r + 1, sc) for r, (uid, sc) in enumerate(ranked)}
        designated = []
        for d in designated_units(case, units):
            uid = d['unit_id']
            prefix = uid[:12] if uid else None
            designated.append({**d, 'chosen': bool(uid) and chosen.get('unit_id') == uid,
                               'candidate_rank': rank_of[prefix][0] if prefix in rank_of else None,
                               'candidate_score': rank_of[prefix][1] if prefix in rank_of else None,
                               'passage_in_span': d['passage'] in span})
        sheet.append({'id': case['id'], 'kind': case['kind'], 'question': case['question'], 'chosen': chosen, 'span': span,
                      'span_chars': len(span), 'designated': designated, 'trace': trace,
                      'reply_characters': len(reply.get('content') or ''), 'truncated': reply.get('truncated'),
                      'visible_reply': reply.get('content') or '', 'expected': case['expected'],
                      'human_review': {f: None for f in fixture['review_fields']}})
    return sheet


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    messages = json.loads((args.product / 'messages.json').read_text())
    units = json.loads((args.product / 'units.json').read_text())
    traces = parse_trace((args.product / 'app.log').read_text(errors='replace'))
    sheet = review(fixture, messages, units, traces)
    out = {'fixture_sha256': hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), 'product': str(args.product), 'traces': len(traces), 'cases': sheet}
    with args.output.open('x') as f:
        json.dump(out, f, ensure_ascii=False, indent=2); f.write('\n')
    for c in sheet:
        if 'error' in c:
            print(c['id'], 'ERROR', c['error']); continue
        des = [(d['chosen'], d['candidate_rank'], d['candidate_score'], d['passage_in_span']) for d in c['designated']]
        t = c['trace']
        print(f"{c['id']:26s} chosen={c['chosen'].get('document')} status={c['chosen'].get('status')} score={c['chosen'].get('score')} "
              f"designated(chosen,rank,score,in_span)={des} span_chars={c['span_chars']} scored={t.get('scored')} "
              f"rerank_ms={t.get('rerank_ms')} extract_ms={t.get('extract_ms')} history_first={t.get('history_first')} reply_chars={c['reply_characters']}")


if __name__ == '__main__':
    main()
