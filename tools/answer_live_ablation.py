#!/usr/bin/env python3
"""Serial component removals on actual captured requests, not a quality gate.

Keeps native instructions/sampling fields except cold greedy generation; uses
the actual 8K slot and full native 2048-token cap. Oracle selection is diagnostic.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil

from answer_bedrock_probe import private_socket, run_server
from answer_live_audit import PASSAGES
from memory_causal_parity import GGUF, GGUF_SHA, ROOT, sha, tokenizer

BOUNDARY = "\nAuthor's current request (not part of the retrieved sources):\n"


def prompt_token_ids(tok, payload):
    # tokenize=True may return BatchEncoding; len then counts mapping fields.
    rendered = tok.apply_chat_template(payload['messages'], tokenize=False, add_generation_prompt=True,
                                       **payload.get('chat_template_kwargs', {}))
    if not isinstance(rendered, str):
        raise ValueError('Expected rendered chat text, not a token mapping')
    ids = tok.encode(rendered, add_special_tokens=False)
    if not isinstance(ids, list) or any(type(token) is not int for token in ids):
        raise ValueError('Expected a flat token-ID list')
    return ids


def variants(payload, question, unit):
    messages = payload['messages']
    if messages[0]['role'] != 'system' or messages[-1]['role'] != 'user':
        raise ValueError('Unexpected captured message shape')
    reference, foreground = messages[-1]['content'].split(BOUNDARY)
    if not foreground.endswith(question) or unit['text'] not in reference:
        raise ValueError('Actual question/designated source not in frozen packet')
    import re
    blocks = list(re.finditer(r'--- SOURCE \d+ \|[^\n]*---\n(.*?)\n--- END SOURCE ---', reference, re.S))
    selected = [m.group() for m in blocks if unit['text'] in m.group(1)]
    if len(selected) != 1:
        raise ValueError('Expected one exact bound oracle unit')
    system, history = messages[0], messages[1:-1]
    choices = (
        ('full', history, messages[-1]['content']),
        ('no_history', [], messages[-1]['content']),
        ('no_previews', history, reference + BOUNDARY + question),
        ('packet_only', [], reference + BOUNDARY + question),
        ('oracle_unit', [], selected[0] + BOUNDARY + question),
    )
    result = []
    for name, prior, content in choices:
        request = copy.deepcopy(payload)
        request.update(stream=False, temperature=0, cache_prompt=False, max_tokens=2048)
        request['messages'] = [copy.deepcopy(system), *copy.deepcopy(prior), {'role': 'user', 'content': content}]
        result.append((name, request))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--index', type=int, choices=[2], default=2)
    args = parser.parse_args()
    payload = json.loads((args.capture / f'request-{args.index}.json').read_text())
    trace = json.loads((args.capture / f'retrieval-{args.index}.json').read_text())
    units = json.loads((args.product / 'units.json').read_text())
    document, passage = PASSAGES[args.index]
    found = [u for u in units if u['document_name'] == document and passage in u['text']]
    if len(found) != 1:
        raise ValueError('Designated unit not uniquely bound')
    arms = variants(payload, trace['query'], found[0])
    tok = tokenizer()
    counts = {}
    for name, request in arms:
        ids = prompt_token_ids(tok, request)
        counts[name] = len(ids)
        if len(ids)+2048 > 8192:
            raise ValueError('Full native reply allowance would exceed actual slot')
    if sha(GGUF) != GGUF_SHA:
        raise ValueError('Pinned model mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (args.capture / f'request-{args.index}.json', args.capture / f'retrieval-{args.index}.json',
                   args.product / 'units.json'):
        shutil.copy2(source, inputs / source.name)
    with private_socket() as sock:
        binary = ROOT / 'build/llama/bin/llama-server'
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '--jinja', '--reasoning-format', 'deepseek', '-np', '2', '-c', '16384', '-b', '256', '-ub', '256',
                   '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'command': command, 'model_sha256': GGUF_SHA, 'binary_sha256': sha(binary),
                    'tool_sha256': sha(Path(__file__)), 'runner_sha256': sha(ROOT / 'tools/answer_bedrock_probe.py'),
                    'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()}, 'prompt_tokens': counts,
                    'slot_tokens': 8192, 'max_new_tokens': 2048, 'index': args.index,
                    'method': 'Actual live request; cold greedy native-penalty component removals; no retrieval/prompt policy change',
                    'limits': 'One fixed question per run; oracle uses known answer-bearing unit. Not a passing product-quality test.'}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        run_server(args.output, command, sock, arms)
        props = json.loads((args.output / 'props.json').read_text())
        if props['default_generation_settings']['n_ctx'] != 8192:
            raise RuntimeError('Actual ablation slot size mismatch; do not interpret results')


if __name__ == '__main__':
    main()
