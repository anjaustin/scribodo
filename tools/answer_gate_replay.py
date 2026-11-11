#!/usr/bin/env python3
"""Replay the anchor gate over every recorded claim; no model.

A gate change is measured before it ships: every claim the pipeline's live runs
recorded (the review sheets under build/answer-pipe-capture, each with the
claim call's answer, the question, the two verified spans and the recorded
verdict) is run through the current gate (build/answer_gate_replay, the gate's
own C code) and the new verdict is set beside the recorded one. The author's
turns are the fixture case's history. Prints every claim whose verdict changed,
then the counts; writes the full table to --output.
"""
import argparse
import glob
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'


def items_from_sheet(sheet, fixture_cases, run):
    """(item for the gate, record) per gated claim of one review sheet."""
    out = []
    for case in sheet['cases']:
        gate = case.get('gate') or {}
        reply = case.get('visible_reply') or ''
        if not gate.get('answer') and not (gate and reply):
            continue
        spans = [s for s in (case.get('span'), (case.get('second') or {}).get('span')) if s]
        turns = [h['content'] for h in fixture_cases.get(case['id'], {}).get('history', []) if h.get('role') == 'user']
        item = {'answer': gate.get('answer') or '', 'question': case['question'], 'spans': spans, 'turns': turns, 'reply': reply}
        out.append((item, {'run': run, 'case': case['id'], 'recorded': gate.get('verdict'), 'answer': gate.get('answer') or '',
                           'recorded_withheld': gate.get('withheld')}))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sheets', default=str(ROOT / 'build/answer-pipe-capture/review-pipeline-*.json'))
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/answer_gate_replay')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    fixture_cases = {c['id']: c for c in json.loads(FIXTURE.read_text())['cases']}
    items, records = [], []
    for path in sorted(glob.glob(args.sheets)):
        run = Path(path).stem.replace('review-pipeline-', '')
        for item, record in items_from_sheet(json.loads(Path(path).read_text()), fixture_cases, run):
            items.append(item)
            records.append(record)
    result = subprocess.run([str(args.binary)], input=json.dumps(items), text=True, capture_output=True, check=True)
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    if len(rows) != len(records):
        sys.exit(f'{len(rows)} results for {len(records)} claims')
    changed = 0
    claims = [(record, row) for record, row in zip(records, rows) if record['answer']]
    for record, row in zip(records, rows):
        record['current'] = row['verdict']
        record['quotations'] = row.get('quotations')
        record['misquoted'] = row.get('misquoted')
        record['changed'] = bool(record['answer']) and row['verdict'] != record['recorded']
        changed += record['changed']
        if record['changed']:
            print(f"{record['run']:14s} {record['case']:28s} {str(record['recorded']):>14s} -> {row['verdict']:<14s} {record['answer'][:90]!r}")
    counts = {}
    for record, _ in claims:
        counts[(record['recorded'], record['current'])] = counts.get((record['recorded'], record['current']), 0) + 1
    print(f'\n{len(claims)} recorded claims, {changed} verdicts changed')
    for (recorded, current), n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f'  {str(recorded):>14s} -> {current:<14s} {n}')
    quoted = [r for r in records if r.get('quotations') is not None]
    flagged = [r for r in quoted if r.get('misquoted')]
    print(f"\n{len(quoted)} recorded replies, {sum(r['quotations'] for r in quoted)} quotation pieces checked, "
          f"{len(flagged)} replies with a quotation the spans do not contain:")
    for r in flagged:
        print(f"  {r['run']:14s} {r['case']:28s} recorded_withheld={r['recorded_withheld']} piece={r['misquoted'][:110]!r}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'claims': records, 'changed': changed, 'misquoted_replies': len(flagged)}, ensure_ascii=False, indent=1) + '\n')


if __name__ == '__main__':
    main()
