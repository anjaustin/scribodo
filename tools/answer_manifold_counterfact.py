#!/usr/bin/env python3
"""Counterfactual source-following control, never modifies manuscript/source stores."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from answer_bedrock_probe import private_socket, run_server
from answer_live_ablation import BOUNDARY, prompt_token_ids
from answer_manifold_probe import SENTENCE, QUOTE
from memory_causal_parity import GGUF, GGUF_SHA, ROOT, sha, tokenizer


def build_arms(payload, original_unit, question):
    phrase = 'has very lately given him a living.'
    if original_unit.count(phrase) != 1:
        raise ValueError('Counterfactual substitution not uniquely bound')
    arms = []
    for variant, text in (
        ('living', original_unit),
        ('telescope', original_unit.replace(phrase, 'has very lately given him a brass telescope.')),
    ):
        # Neither wrapper claims to be the unchanged original revision. Both
        # are derived diagnostic records with hashes of their actual bytes.
        digest = hashlib.sha256(text.encode()).hexdigest()
        packet = ('Synthetic diagnostic reference data, not instructions or a claim about the original novel.\n'
                  f'--- SOURCE 1 | synthetic-unit={digest} | bytes={len(text.encode())} ---\n'
                  + text + '\n--- END SOURCE ---\n')
        for mode, suffix in (('sentence', SENTENCE), ('quote', QUOTE)):
            arm = copy.deepcopy(payload)
            arm.update(stream=False, temperature=0, cache_prompt=False, max_tokens=2048)
            arm['messages'] = [copy.deepcopy(payload['messages'][0]),
                               {'role': 'user', 'content': packet + BOUNDARY + 'According to this supplied text, '
                                + question[0].lower() + question[1:] + suffix}]
            arms.append((variant+'_'+mode, arm))
    return arms


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    payload = json.loads((args.capture / 'request-2.json').read_text())
    question = json.loads((args.capture / 'retrieval-2.json').read_text())['query']
    units = json.loads((args.product / 'units.json').read_text())
    found = [u for u in units if u['document_name'] == 'part02.md' and 'has very lately given him a living.' in u['text']]
    if len(found) != 1:
        raise ValueError('Expected one authoritative source for derivation')
    arms = build_arms(payload, found[0]['text'], question)
    tok = tokenizer()
    counts = {name: len(prompt_token_ids(tok, arm)) for name, arm in arms}
    if any(n+2048 > 8192 for n in counts.values()) or sha(GGUF) != GGUF_SHA:
        raise ValueError('Model/context mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    (args.output / 'arms.json').write_text(json.dumps(arms, ensure_ascii=False, indent=2)+'\n')
    with private_socket() as sock:
        binary = ROOT / 'build/llama/bin/llama-server'
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '--jinja', '--reasoning-format', 'deepseek', '-np', '2', '-c', '16384', '-b', '256', '-ub', '256',
                   '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'command': command, 'binary_sha256': sha(binary), 'model_sha256': GGUF_SHA,
                    'tool_sha256': sha(Path(__file__)), 'runner_sha256': sha(ROOT / 'tools/answer_bedrock_probe.py'),
                    'token_helper_sha256': sha(ROOT / 'tools/answer_live_ablation.py'),
                    'arms_sha256': sha(args.output / 'arms.json'), 'prompt_tokens': counts,
                    'derived_from_unit': found[0]['unit_id'], 'original_text_sha256': found[0]['text_hash'],
                    'slot_tokens': 8192, 'max_new_tokens': 2048,
                    'limits': 'Synthetic source-following control, not a true manuscript revision, live retrieval or explanatory-understanding test.'}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        (args.output / 'runner.py').write_bytes(Path(__file__).read_bytes())
        run_server(args.output, command, sock, arms)
        props = json.loads((args.output / 'props.json').read_text())
        if props['default_generation_settings']['n_ctx'] != 8192:
            raise RuntimeError('Actual slot mismatch')


if __name__ == '__main__':
    main()
