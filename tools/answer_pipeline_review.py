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
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / os.environ.get('SCRIBODO_BOUNDARY_FIXTURE', 'tests/fixtures/answer_boundary/questions.json')  # a second manuscript's fixture by environment
TRACE = re.compile(r'ANSWER_PIPELINE candidates=(\d+) hydrated=(\d+) scored=(\d+) failures=(\d+) top=(\S+) score=([0-9.]+) status=(\w+) '
                   r'(?:second=(\S+) second_score=([0-9.]+) second_status=(\S+) )?'
                   r'(?:parts=(\d+) top_part=(\d+) second_part=(\d+) )?'
                   r'rerank_ms=([0-9.]+) extract_ms=([0-9.]+) request_bytes=(\d+)(?: history_first=(\d+))?')
# Scores: one line per question part since cycle 3 ("part=N"); earlier runs wrote one unlabeled line (part 1).
SCORES = re.compile(r'ANSWER_PIPELINE_SCORES(?: part=(\d+))?((?: [0-9a-f]{12}:[0-9.]+)*)')
RECEIPT = re.compile(r'--- VERIFIED SOURCE \| (?:unit=([0-9a-f]+) \| document=([^|]+) \| score=([0-9.]+) \| status=(\w+)|none \| status=(\w+))')
# The free-text fields run to the line's final " ---" (llm/answer_receipt.h): take the whole line, not the first " ---".
GATE = re.compile(r'^--- ANSWER GATE \| verdict=(\w+) \| withheld=(\d) \| answer=(.*) ---$', re.M)
QUOTES = re.compile(r'^--- ANSWER QUOTES \| checked=(\d+) \| misquoted=(.*) ---$', re.M)
RELATIONS = re.compile(r'^--- ANSWER RELATIONS \| checked=(\d+) \| unsupported=(.*) ---$', re.M)
SECOND = re.compile(r'--- VERIFIED SOURCE 2 \| unit=([0-9a-f]+) \| document=([^|]+) \| score=([0-9.]+) \| status=(\w+)(?: \| part=(\d+))? ---\n(.*?)\n(?=---|\Z)', re.S)
PART = re.compile(r'\| part=(\d+) \|')


def parse_trace(log_text):
    """(trace, scores) pairs in question order: scores is part 1's list (empty for runs without a score line) and
    trace['part_scores'] holds every part's list, keyed by part number."""
    traces = []
    pending = None

    def close():
        if pending:
            traces.append((pending, pending['part_scores'].get(1, [])))
    for line in log_text.splitlines():
        m = TRACE.search(line)
        if m:
            close()
            pending = {'candidates': int(m[1]), 'hydrated': int(m[2]), 'scored': int(m[3]), 'failures': int(m[4]), 'top': m[5],
                       'score': float(m[6]), 'status': m[7], 'second': m[8], 'second_score': float(m[9]) if m[9] else None,
                       'second_status': m[10], 'parts': int(m[11]) if m[11] else None, 'top_part': int(m[12]) if m[12] else None,
                       'second_part': int(m[13]) if m[13] else None, 'rerank_ms': float(m[14]), 'extract_ms': float(m[15]),
                       'request_bytes': int(m[16]), 'history_first': int(m[17]) if m[17] else None, 'part_scores': {}}
            continue
        s = SCORES.search(line)
        if s and pending:
            part = int(s[1]) if s[1] else 1
            pending['part_scores'][part] = [(p.split(':')[0], float(p.split(':')[1])) for p in s[2].split()]
    close()
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
        first_part = receipt.split('--- VERIFIED SOURCE 2', 1)[0]
        span = first_part.split('---\n', 1)[1].strip() if '---\n' in first_part else ''
        g = GATE.search(receipt)
        gate = {'verdict': g[1], 'withheld': g[2] == '1', 'answer': g[3]} if g else None
        qm = QUOTES.search(receipt)
        if gate is not None and qm:
            gate['quotations'] = int(qm[1])
            gate['misquoted'] = qm[2] or None
        rm = RELATIONS.search(receipt)
        if gate is not None and rm:
            gate['relations'] = int(rm[1])
            gate['unsupported'] = rm[2] or None
        s2 = SECOND.search(receipt)
        second = {'unit_id': s2[1], 'document': s2[2].strip(), 'score': float(s2[3]), 'status': s2[4],
                  'part': int(s2[5]) if s2[5] else None, 'span': s2[6].strip()} if s2 else None
        first_part = PART.search(first_part)
        chosen['part'] = int(first_part[1]) if first_part else None
        trace, scores = trace_for(q['seq'] + 1)
        ranked = sorted(scores, key=lambda s: -s[1])
        rank_of = {uid: (r + 1, sc) for r, (uid, sc) in enumerate(ranked)}
        by_part = {part: {uid: (r + 1, sc) for r, (uid, sc) in enumerate(sorted(lst, key=lambda s: -s[1]))}
                   for part, lst in (trace.get('part_scores') or {}).items()}
        designated = []
        for d in designated_units(case, units):
            uid = d['unit_id']
            prefix = uid[:12] if uid else None
            designated.append({**d, 'chosen': bool(uid) and chosen.get('unit_id') == uid,
                               'chosen_second': bool(uid) and bool(second) and second['unit_id'] == uid,
                               'candidate_rank': rank_of[prefix][0] if prefix in rank_of else None,
                               'candidate_score': rank_of[prefix][1] if prefix in rank_of else None,
                               'candidate_rank_by_part': {part: r[prefix][0] for part, r in by_part.items() if prefix in r},
                               'passage_in_span': d['passage'] in span or (bool(second) and d['passage'] in second['span'])})
        sheet.append({'id': case['id'], 'kind': case['kind'], 'question': case['question'], 'chosen': chosen, 'span': span, 'second': second, 'gate': gate,
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
        des = [(d['chosen'] or ('2nd' if d['chosen_second'] else False), d['candidate_rank_by_part'] or d['candidate_rank'], d['candidate_score'], d['passage_in_span']) for d in c['designated']]
        t = c['trace']
        print(f"{c['id']:26s} chosen={c['chosen'].get('document')} status={c['chosen'].get('status')} score={c['chosen'].get('score')} "
              f"designated(chosen,rank,score,in_span)={des} span_chars={c['span_chars']} scored={t.get('scored')} "
              f"parts={t.get('parts')} slot_parts=({c['chosen'].get('part')},{c['second']['part'] if c['second'] else None}) "
              f"rerank_ms={t.get('rerank_ms')} extract_ms={t.get('extract_ms')} history_first={t.get('history_first')} reply_chars={c['reply_characters']} gate={c.get('gate')}")


if __name__ == '__main__':
    main()
