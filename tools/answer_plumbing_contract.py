#!/usr/bin/env python3
"""Same-message deployment controls; no prompt edits, model switch or oracle.

Changes only the documented thinking flag and three vendor-core sampler fields.
Other sampler fields remain native: this is not full Transformers sampler parity.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil

from answer_bedrock_probe import private_socket, run_server
from answer_live_ablation import prompt_token_ids
from answer_plumbing_arithmetic import backend_flags
from memory_causal_parity import GGUF, GGUF_SHA, ROOT, SNAPSHOT, sha, tokenizer

INDICES = (2, 6)  # Fully admitted factual failure and unavailable-fact failure.
CARD = 'https://huggingface.co/LiquidAI/LFM3-1.7B/blob/0dc4268b561499c648c7ba7c9466bf3d8d01517a/README.md'


def variants(payload, index):
    if payload.get('chat_template_kwargs') != {'enable_thinking': False}:
        raise ValueError('Unexpected native thinking configuration')
    arms = []
    for name, vendor_sampling, thinking in (
        ('native', False, False), ('vendor_sampling', True, False),
        ('thinking', False, True), ('vendor_both', True, True),
    ):
        arm = copy.deepcopy(payload)
        arm.update(stream=False, cache_prompt=False)
        if vendor_sampling:
            arm.update(temperature=.1, top_k=50, repeat_penalty=1.1)
        if thinking:
            arm['chat_template_kwargs']['enable_thinking'] = True
        arms.append((f'q{index}_{name}', arm))
    return arms


def budget_variants(payload, index=6, conditions=('native', 'vendor_both')):
    selected = dict(variants(payload, index))
    arms = []
    for condition in conditions:
        for cap in (2048, 8192):
            arm = copy.deepcopy(selected[f'q{index}_{condition}'])
            arm['max_tokens'] = cap
            arms.append((f'q{index}_{condition}_cap{cap}', arm))
    return arms


def remainder_variants(payload, tok):
    native = variants(payload, 6)[0][1]
    remaining = 8192-len(prompt_token_ids(tok, native))-256
    if remaining <= native['max_tokens']:
        raise ValueError('No extra physical reply allowance for this control')
    extended = copy.deepcopy(native)
    extended['max_tokens'] = remaining
    return [('q6_native_cap2048', native), ('q6_native_remainder', extended)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--messages', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phase', choices=['contract', 'budget', 'thinking_budget', 'backend', 'remainder'], default='contract')
    args = parser.parse_args()
    rows = {r['seq']: r for r in json.loads(args.messages.read_text())}
    tok = tokenizer(); arms = []; counts = {}
    indices = (6,) if args.phase in ('budget', 'remainder') else INDICES
    slot_tokens, slots = (8192, 2) if args.phase in ('contract', 'backend', 'remainder') else (16384, 1)
    for index in indices:
        payload = json.loads((args.capture / f'request-{index}.json').read_text())
        trace = json.loads((args.capture / f'retrieval-{index}.json').read_text())
        if rows[403+2*index]['content'] != trace['query'] or payload['max_tokens'] != 2048 or payload['seed'] != 42:
            raise ValueError('Native question, seed or reply allowance mismatch')
        if args.phase == 'contract':
            prepared = variants(payload, index)
        elif args.phase == 'backend':
            prepared = variants(payload, index)[:1]
        elif args.phase == 'remainder':
            prepared = remainder_variants(payload, tok)
        else:
            conditions = ('thinking',) if args.phase == 'thinking_budget' else ('native', 'vendor_both')
            prepared = budget_variants(payload, index, conditions)
        for name, arm in prepared:
            ids = prompt_token_ids(tok, arm)
            if len(ids)+arm['max_tokens'] > slot_tokens:
                raise ValueError('Full native reply allowance does not fit actual slot')
            counts[name] = len(ids); arms.append((name, arm))
    if sha(GGUF) != GGUF_SHA:
        raise ValueError('Pinned model mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for index in indices:
        for kind in ('request', 'retrieval'):
            source = args.capture / f'{kind}-{index}.json'; shutil.copy2(source, inputs / source.name)
    for source in (args.messages, Path(__file__), SNAPSHOT / 'generation_config.json', SNAPSHOT / 'chat_template.jinja'):
        shutil.copy2(source, inputs / source.name)
    (args.output / 'arms.json').write_text(json.dumps(arms, ensure_ascii=False, indent=2)+'\n')
    with private_socket() as sock:
        binary = ROOT / 'build/llama/bin/llama-server'
        selected_backend = 'cpu_bf16' if args.phase == 'backend' else 'metal_bf16'
        command = [str(binary), '-m', str(GGUF), *backend_flags(selected_backend), '-fa', 'off',
                   '--jinja', '--reasoning-format', 'deepseek', '-np', str(slots), '-c', str(slots*slot_tokens), '-b', '256', '-ub', '256',
                   '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'command': command, 'model_sha256': GGUF_SHA, 'binary_sha256': sha(binary),
                    'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()}, 'arms_sha256': sha(args.output / 'arms.json'),
                    'runner_sha256': sha(ROOT / 'tools/answer_bedrock_probe.py'), 'model_card': CARD,
                    'token_helper_sha256': sha(ROOT / 'tools/answer_live_ablation.py'),
                    'backend_helper_sha256': sha(ROOT / 'tools/answer_plumbing_arithmetic.py'),
                    'prompt_tokens': counts, 'slot_tokens': slot_tokens, 'slots': slots, 'indices': indices,
                    'phase': args.phase,
                    'generation_http_timeout_seconds': 180 if args.phase == 'contract' else 600,
                    'max_new_tokens': {name: arm['max_tokens'] for name, arm in arms},
                    'limits': 'Same-message diagnostic controls. Native top_p/min_p and llama penalty scope retained; vendor_sampling means the three documented core fields only. Budget phase deliberately uses one 16K slot, not actual native 8K. Stochastic seed 42, not repeated-trial quality certification.'}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        run_server(args.output, command, sock, arms, timeout=180 if args.phase == 'contract' else 600)
        props = json.loads((args.output / 'props.json').read_text())
        if props['default_generation_settings']['n_ctx'] != slot_tokens or props['total_slots'] != slots:
            raise RuntimeError('Actual slot mismatch')


if __name__ == '__main__':
    main()
