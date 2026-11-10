#!/usr/bin/env python3
"""Audit public-fixture live captures; no inference, SQL writes or quality score."""
import argparse
import hashlib
import json
from pathlib import Path
import re

# Exact manuscript passages, not arbitrary occurrences of the answer phrase.
PASSAGES = (
    ('part01.md', 'have you heard that Netherfield Park is let at last?'),
    ('part01.md', 'A single man of large fortune; four or five thousand a year.'),
    ('part02.md', 'has very lately given him a living.'),
    ('part04.md', 'Colonel Fitzwilliam, who led the way, was about thirty'),
    ('part05.md', 'I made a point of her having two men-servants go with her.'),
    ('part05.md', 'They are going to be encamped near Brighton;'),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(capture, product):
    units = json.loads((product / 'units.json').read_text())
    messages = {row['seq']: row for row in json.loads((product / 'messages.json').read_text())}
    by_id = {u['unit_id']: u for u in units}
    failures, cases = [], []
    for unit in units:
        if (not unit['source_hash_current'] or not unit['source_bytes_current'] or
                hashlib.sha256(unit['text'].encode()).hexdigest() != unit['text_hash']):
            failures.append({'unit': unit['unit_id'], 'failure': 'source integrity'})
    for index in range(7):
        trace = json.loads((capture / f'retrieval-{index}.json').read_text())
        payload = json.loads((capture / f'request-{index}.json').read_text())
        outcome = json.loads((capture / f'outcome-{index}.json').read_text())
        raw = (capture / f'content-{index}.txt').read_text()
        reply = messages[404+2*index]
        if not outcome.get('capture_complete'):
            failures.append({'index': index, 'failure': 'incomplete capture'})
        if messages[403+2*index]['content'] != trace['query']:
            failures.append({'index': index, 'failure': 'question identity'})
        parts, reasons = [], []
        finishes = []
        for line in (capture / f'stream-{index}.sse').read_text().splitlines():
            if not line.startswith('data:') or line[5:].strip() == '[DONE]':
                continue
            event = json.loads(line[5:])
            for choice in event.get('choices', []):
                delta = choice.get('delta', {})
                parts.append(delta.get('content') or '')
                reasons.append(delta.get('reasoning_content') or '')
                if choice.get('finish_reason'):
                    finishes.append(choice['finish_reason'])
        if ''.join(parts) != raw:
            failures.append({'index': index, 'failure': 'SSE content accumulation'})
        ranks, order = {}, []
        for channel in ('lexical', 'semantic'):
            for rank, candidate in enumerate(trace[channel], 1):
                uid = candidate['unit_id']
                unit = by_id.get(uid)
                if (unit is None or candidate['text_hash'] != unit['text_hash'] or
                        unit['scope_id'] != trace['scope_id'] or unit['generation'] != trace['generation'] or
                        (unit['source_kind'] == 'chat' and unit['sequence'] >= trace['before'])):
                    failures.append({'index': index, 'channel': channel, 'unit': uid, 'failure': 'locator binding/cutoff'})
                if uid not in ranks:
                    ranks[uid] = {}; order.append(uid)
                ranks[uid][channel] = rank
        # Production RRF: zero-based rank and K=1, hence reciprocal one-based rank.
        fused = sorted(order, key=lambda uid: -sum(1/r for r in ranks[uid].values()))
        admitted = re.findall(r'\| unit=([0-9a-f]{64}) \|', reply['memory_sources'] or '')
        source_details = []
        for uid in fused:
            unit = by_id.get(uid)
            if unit is None:
                continue  # Missing locator is already an explicit failure above.
            source_details.append({'unit_id': uid, 'document': unit['document_name'], 'sequence': unit['sequence'],
                                   'ranks': ranks[uid], 'fused_rank': fused.index(uid)+1,
                                   'admitted': uid in admitted, 'text_bytes': len(unit['text'].encode())})
        designated = None
        if index < len(PASSAGES):
            document, passage = PASSAGES[index]
            found = [u for u in units if u['document_name'] == document and passage in u['text']]
            if len(found) != 1:
                raise ValueError(f'Expected one designated passage for index {index}, got {len(found)}')
            unit = found[0]; uid = unit['unit_id']
            designated = {'unit_id': uid, 'document': document, 'passage': passage, 'ranks': ranks.get(uid, {}),
                          'fused_rank': fused.index(uid)+1 if uid in fused else None, 'admitted': uid in admitted,
                          'complete_unit_in_final_request': any(unit['text'] in m['content'] for m in payload['messages']),
                          'passage_in_final_request': any(passage in m['content'] for m in payload['messages'])}
        cases.append({'index': index, 'question': trace['query'], 'cutoff': trace['before'], 'outcome': outcome,
                      'finish_reasons': finishes, 'reasoning_delta_characters': len(''.join(reasons)),
                      'raw_equals_visible': raw == reply['content'], 'raw_characters': len(raw),
                      'raw_closing_think_tags': raw.count('</think>'),
                      'suffix_after_last_think_equals_visible': raw.rsplit('</think>', 1)[-1].lstrip() == reply['content'] if '</think>' in raw else None,
                      'visible_characters': len(reply['content']), 'admitted_units': admitted,
                      'designated': designated, 'fused_candidates': source_details})
    inputs = [*capture.iterdir(), product / 'units.json', product / 'messages.json', product / 'app.log']
    return {'source_units': len(units), 'integrity_failures': failures, 'cases': cases, 'tool_sha256': digest(Path(__file__)),
            'input_sha256': {str(p): digest(p) for p in inputs if p.is_file()},
            'limits': 'SQL comparison flags were exported before cleanup. Fusion reconstructed from captured ranks, not an admission-decision trace; candidate epochs were not recorded. No full-answer quality certification.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.capture, args.product)
    with args.output.open('x') as file:
        json.dump(result, file, ensure_ascii=False, indent=2); file.write('\n')
    print(json.dumps({'source_units': result['source_units'], 'integrity_failures': result['integrity_failures'],
                      'designated': [c['designated'] for c in result['cases']]}, indent=2))
    if result['integrity_failures']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
