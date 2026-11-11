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
        if not gate.get('answer'):
            continue
        spans = [s for s in (case.get('span'), (case.get('second') or {}).get('span')) if s]
        turns = [h['content'] for h in fixture_cases.get(case['id'], {}).get('history', []) if h.get('role') == 'user']
        item = {'answer': gate['answer'], 'question': case['question'], 'spans': spans, 'turns': turns}
        out.append((item, {'run': run, 'case': case['id'], 'recorded': gate.get('verdict'), 'answer': gate['answer']}))
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
    verdicts = [json.loads(line)['verdict'] for line in result.stdout.splitlines() if line.strip()]
    if len(verdicts) != len(records):
        sys.exit(f'{len(verdicts)} verdicts for {len(records)} claims')
    changed = 0
    for record, verdict in zip(records, verdicts):
        record['current'] = verdict
        record['changed'] = verdict != record['recorded']
        changed += record['changed']
        if record['changed']:
            print(f"{record['run']:14s} {record['case']:28s} {record['recorded']:>14s} -> {verdict:<14s} {record['answer'][:90]!r}")
    counts = {}
    for record in records:
        counts[(record['recorded'], record['current'])] = counts.get((record['recorded'], record['current']), 0) + 1
    print(f'\n{len(records)} recorded claims, {changed} verdicts changed')
    for (recorded, current), n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f'  {recorded:>14s} -> {current:<14s} {n}')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'claims': records, 'changed': changed}, ensure_ascii=False, indent=1) + '\n')


if __name__ == '__main__':
    main()
