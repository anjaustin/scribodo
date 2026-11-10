#!/usr/bin/env python3
"""Isolate source selection from task execution on the actual captured income request.

Manifold cycle step 1. The exact live packet is compared with diagnostic packets in
which one admitted source block is replaced by the designated opening original,
carrying that unit's real locator (document, revision, SHA-256, unit id, byte
range). All author history, the other sources, the attachment preview and the
question stay byte-identical. Cold greedy decoding, native 2048-token cap.
Oracle passage knowledge is used only here, never in a production ranker.
"""
import argparse
import copy
import json
from pathlib import Path
import re
import shutil

from answer_bedrock_probe import private_socket, run_server
from answer_live_ablation import BOUNDARY, prompt_token_ids
from answer_live_audit import PASSAGES
from answer_manifold_probe import SENTENCE
from memory_causal_parity import GGUF, GGUF_SHA, ROOT, sha, tokenizer

INDEX = 1  # The income question, fixed before inspecting any reply.
BLOCK = re.compile(r'--- SOURCE (\d+) \| ([^\n]*?) \| unit=([0-9a-f]{64}) \| bytes=(\d+)\+(\d+) ---\n(.*?)\n--- END SOURCE ---', re.S)


def blocks(reference):
    found = list(BLOCK.finditer(reference))
    if not found:
        raise ValueError('No source blocks in the captured packet')
    return found


def swap_block(reference, match, unit):
    """Replace one block's header locator and text with the designated unit's own."""
    number, prefix, _, _, _, _ = match.groups()
    header = f"--- SOURCE {number} | {prefix} | unit={unit['unit_id']} | bytes={unit['byte_start']}+{len(unit['text'].encode())} ---"
    return reference[:match.start()] + header + '\n' + unit['text'] + '\n--- END SOURCE ---' + reference[match.end():]


def build_arms(payload, question, designated, quoted_unit_id, units):
    messages = payload['messages']
    if messages[0]['role'] != 'system' or messages[-1]['role'] != 'user':
        raise ValueError('Unexpected captured message shape')
    reference, foreground = messages[-1]['content'].split(BOUNDARY)
    if not foreground.endswith(question) or designated['text'] in reference:
        raise ValueError('Question missing or designated original already present')
    found = blocks(reference)
    by_unit = {m.group(3): m for m in found}
    if quoted_unit_id not in by_unit:
        raise ValueError('The source quoted by the native reply is not an admitted block')
    quoted = by_unit[quoted_unit_id]
    last = found[-1]
    if last.group(3) == quoted_unit_id:
        raise ValueError('Quoted and last blocks coincide; second position arm is not distinct')
    # The designated unit shares part01.md with the quoted block; both carry the
    # same document/revision/SHA prefix, so only the unit id and bytes change.
    quoted_doc = units[quoted_unit_id]
    if quoted_doc['document_name'] != designated['document_name'] or quoted_doc['revision_id'] != designated['revision_id']:
        raise ValueError('Quoted block is not from the designated document revision; header prefix would be wrong')
    variants = {
        'baseline': reference,
        'swap_quoted': swap_block(reference, quoted, designated),
    }
    if units[last.group(3)]['revision_id'] == designated['revision_id']:
        variants['swap_last'] = swap_block(reference, last, designated)
    arms = []
    for name, ref in variants.items():
        for suffix_name, suffix in (('', ''), ('_one_sentence', SENTENCE)):
            request = copy.deepcopy(payload)
            request.update(stream=False, temperature=0, cache_prompt=False, max_tokens=2048)
            request['messages'][-1]['content'] = ref + BOUNDARY + foreground + suffix
            arms.append((name + suffix_name, request))
    return arms, {k: v for k, v in variants.items()}


def verify(arms, variants, designated, quoted_unit_id, payload):
    checks = {}
    base_hist = payload['messages'][1:-1]
    for name, request in arms:
        content = request['messages'][-1]['content']
        reference = content.split(BOUNDARY)[0]
        found = blocks(reference)
        swapped = not name.startswith('baseline')
        checks[name] = {
            'history_identical': request['messages'][1:-1] == base_hist,
            'system_identical': request['messages'][0] == payload['messages'][0],
            'source_count': len(found),
            'numbering_unchanged': [m.group(1) for m in found] == [str(i) for i in range(1, len(found)+1)],
            'designated_text_present': designated['text'] in reference,
            'designated_locator_present': f"unit={designated['unit_id']} | bytes={designated['byte_start']}+{len(designated['text'].encode())}" in reference,
            'quoted_unit_present': quoted_unit_id in reference,
            'expected_designated': swapped,
        }
        c = checks[name]
        ok = (c['history_identical'] and c['system_identical'] and c['numbering_unchanged'] and
              c['designated_text_present'] == swapped and c['designated_locator_present'] == swapped and
              (c['quoted_unit_present'] == (not name.startswith('swap_quoted'))))
        c['ok'] = ok
        if not ok:
            raise ValueError(f'Packet verification failed for {name}: {c}')
    return checks


def token_positions(tok, arms):
    ids = {name: prompt_token_ids(tok, request) for name, request in arms}
    base = ids['baseline']
    rows = {}
    for name, seq in ids.items():
        first = next((i for i, (a, b) in enumerate(zip(base, seq)) if a != b), min(len(base), len(seq)))
        rows[name] = {'prompt_tokens': len(seq), 'first_changed_position': first if seq != base else None,
                      'length_delta': len(seq) - len(base)}
        if len(seq) + 2048 > 8192:
            raise ValueError(f'{name}: full native reply allowance exceeds the actual 8K slot')
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quoted-unit', required=True,
                        help='Admitted unit id the native reply quoted as income evidence (prospectively chosen)')
    parser.add_argument('--note', default='', help='Run conditions worth recording, e.g. concurrent CPU arm')
    args = parser.parse_args()
    payload = json.loads((args.capture / f'request-{INDEX}.json').read_text())
    trace = json.loads((args.capture / f'retrieval-{INDEX}.json').read_text())
    rows = {r['seq']: r for r in json.loads((args.product / 'messages.json').read_text())}
    if rows[403+2*INDEX]['content'] != trace['query']:
        raise ValueError('Frozen question identity mismatch')
    units = {u['unit_id']: u for u in json.loads((args.product / 'units.json').read_text())}
    document, passage = PASSAGES[INDEX]
    designated = [u for u in units.values() if u['document_name'] == document and passage in u['text']]
    if len(designated) != 1:
        raise ValueError('Designated original not unique')
    designated = designated[0]
    if units[args.quoted_unit]['text'] not in payload['messages'][-1]['content']:
        raise ValueError('Quoted unit text is not in the captured packet')
    arms, variants = build_arms(payload, trace['query'], designated, args.quoted_unit, units)
    checks = verify(arms, variants, designated, args.quoted_unit, payload)
    tok = tokenizer()
    positions = token_positions(tok, arms)
    if sha(GGUF) != GGUF_SHA:
        raise ValueError('Pinned model mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (args.capture / f'request-{INDEX}.json', args.capture / f'retrieval-{INDEX}.json',
                   args.product / 'units.json', args.product / 'messages.json', Path(__file__)):
        shutil.copy2(source, inputs / source.name)
    (args.output / 'arms.json').write_text(json.dumps(arms, ensure_ascii=False, indent=2)+'\n')
    with private_socket() as sock:
        binary = ROOT / 'build/llama/bin/llama-server'
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '--jinja', '--reasoning-format', 'deepseek', '-np', '2', '-c', '16384', '-b', '256', '-ub', '256',
                   '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'command': command, 'model_sha256': GGUF_SHA, 'binary_sha256': sha(binary),
                    'tool_sha256': sha(Path(__file__)), 'runner_sha256': sha(ROOT / 'tools/answer_bedrock_probe.py'),
                    'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()}, 'arms_sha256': sha(args.output / 'arms.json'),
                    'question': trace['query'], 'designated_unit': designated['unit_id'], 'quoted_unit': args.quoted_unit,
                    'packet_checks': checks, 'prompt_tokens': positions, 'slot_tokens': 8192, 'max_new_tokens': 2048,
                    'method': 'Exact live income packet versus the same packet with one admitted block replaced by the designated opening original and its real locator; cold greedy; plain and one-sentence task conditions',
                    'limits': 'One question. Replacement positions are the block the native reply quoted and the last admitted block. Oracle knowledge selects the inserted unit; nothing here is a production ranker. Not an answer-quality certification.',
                    'note': args.note}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
        run_server(args.output, command, sock, arms)
        props = json.loads((args.output / 'props.json').read_text())
        if props['default_generation_settings']['n_ctx'] != 8192:
            raise RuntimeError('Actual slot mismatch; results must not be interpreted')


if __name__ == '__main__':
    main()
