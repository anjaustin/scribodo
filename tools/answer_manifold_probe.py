#!/usr/bin/env python3
"""Predeclared LMM uncertainty controls on one actual public-fixture request.

Diagnostic only: no production prompts, source selection or models are changed.
Complete responses use the native 2048-token cap. Run serially with other models.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil

from answer_bedrock_probe import private_socket, run_server
from answer_live_ablation import variants as component_variants
from answer_live_audit import PASSAGES
from memory_causal_parity import GGUF, GGUF_SHA, ROOT, sha, tokenizer

SENTENCE = '\nAnswer in one sentence.'
QUOTE = ('\nReply only with one verbatim sentence from the supplied original that directly answers '
         'the current question; do not add a source label, definition, explanation, or other text.')


def neutral_text(tok, original, theme):
    target = len(tok.encode(original, add_special_tokens=False))
    seed = {
        'weather': 'The morning was quiet. A breeze moved the clouds. The afternoon remained calm. ',
        'room': 'The room was quiet. A chair stood beside the window. The curtains were plain. ',
    }[theme]
    ids = tok.encode(seed*(target+1), add_special_tokens=False)[:target]
    text = tok.decode(ids, skip_special_tokens=False)
    if len(tok.encode(text, add_special_tokens=False)) != target:
        raise ValueError('Neutral history failed per-message token-count match')
    return text


def build_arms(payload, question, unit, tok):
    components = dict(component_variants(payload, question, unit))
    baseline = components['full']
    arms = [('baseline', baseline)]
    no_penalty = copy.deepcopy(baseline)
    no_penalty['repeat_penalty'] = 1.0
    arms.append(('penalty_1', no_penalty))
    for theme in ('weather', 'room'):
        neutral = copy.deepcopy(baseline)
        for message in neutral['messages'][1:-1]:
            message['content'] = neutral_text(tok, message['content'], theme)
        arms.append(('neutral_'+theme, neutral))
    for name, original, constraint in (
        ('one_sentence', baseline, SENTENCE),
        ('quote_only', baseline, QUOTE),
        ('oracle_one_sentence', components['oracle_unit'], SENTENCE),
    ):
        arm = copy.deepcopy(original)
        arm['messages'][-1]['content'] += constraint
        arms.append((name, arm))
    counts = {}
    for name, arm in arms:
        rendered = tok.apply_chat_template(arm['messages'], tokenize=False, add_generation_prompt=True, enable_thinking=False)
        ids = tok.encode(rendered, add_special_tokens=False)
        counts[name] = len(ids)
        if len(ids)+2048 > 8192:
            raise ValueError('Full reply allowance exceeds actual live slot')
    for name in ('neutral_weather', 'neutral_room'):
        if counts[name] != counts['baseline']:
            raise ValueError('Neutral full-template token count differs from baseline')
    return arms, counts


def build_transfer_arms(payloads, tok):
    arms = []
    for name, index, suffix in (
        ('neutral_suffix_question', 2, '\nThis is a question.'),
        ('neutral_suffix_end', 2, '\nThe question ends here.'),
        ('one_sentence_replicate', 2, SENTENCE),
        *((f'question_{i}_one_sentence', i, SENTENCE) for i in (0, 1, 3, 4, 5, 6)),
    ):
        arm = copy.deepcopy(payloads[index])
        arm.update(stream=False, temperature=0, cache_prompt=False, max_tokens=2048)
        arm['messages'][-1]['content'] += suffix
        arms.append((name, arm))
    def count(payload):
        rendered = tok.apply_chat_template(payload['messages'], tokenize=False, add_generation_prompt=True, enable_thinking=False)
        return len(tok.encode(rendered, add_special_tokens=False))
    counts = {name: count(arm) for name, arm in arms}
    for name in ('neutral_suffix_question', 'neutral_suffix_end'):
        if counts[name] != counts['one_sentence_replicate']:
            raise ValueError('Non-directive suffix count differs from treatment')
    if any(n+2048 > 8192 for n in counts.values()):
        raise ValueError('Full reply allowance exceeds actual slot')
    return arms, counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phase', choices=['mechanism', 'transfer'], default='mechanism')
    args = parser.parse_args()
    payload = json.loads((args.capture / 'request-2.json').read_text())
    trace = json.loads((args.capture / 'retrieval-2.json').read_text())
    rows = {r['seq']: r for r in json.loads((args.product / 'messages.json').read_text())}
    if rows[407]['content'] != trace['query']:
        raise ValueError('Frozen question identity mismatch')
    document, passage = PASSAGES[2]
    units = json.loads((args.product / 'units.json').read_text())
    found = [u for u in units if u['document_name'] == document and passage in u['text']]
    if len(found) != 1:
        raise ValueError('Designated original not unique')
    tok = tokenizer()
    if args.phase == 'mechanism':
        arms, counts = build_arms(payload, trace['query'], found[0], tok)
        capture_indices = (2,)
    else:
        payloads = {}
        for index in range(7):
            current_trace = json.loads((args.capture / f'retrieval-{index}.json').read_text())
            if rows[403+2*index]['content'] != current_trace['query']:
                raise ValueError('Cross-question capture identity mismatch')
            payloads[index] = json.loads((args.capture / f'request-{index}.json').read_text())
        arms, counts = build_transfer_arms(payloads, tok)
        capture_indices = range(7)
    if sha(GGUF) != GGUF_SHA:
        raise ValueError('Pinned model mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    sources = [args.product / 'units.json', args.product / 'messages.json', Path(__file__)]
    for index in capture_indices:
        sources.extend((args.capture / f'request-{index}.json', args.capture / f'retrieval-{index}.json'))
    for source in sources:
        shutil.copy2(source, inputs / source.name)
    with private_socket() as sock:
        binary = ROOT / 'build/llama/bin/llama-server'
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '--jinja', '--reasoning-format', 'deepseek', '-np', '2', '-c', '16384', '-b', '256', '-ub', '256',
                   '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'command': command, 'model_sha256': GGUF_SHA, 'binary_sha256': sha(binary),
                    'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()}, 'prompt_tokens': counts,
                    'helper_sha256': {n: sha(ROOT / 'tools' / n) for n in ('answer_bedrock_probe.py', 'answer_live_ablation.py', 'memory_causal_parity.py')},
                    'slot_tokens': 8192, 'max_new_tokens': 2048, 'question': trace['query'],
                    'phase': args.phase,
                    'method': 'Same-route cold greedy native-penalty baseline and single-factor interventions',
                    'limits': ('One factual question; two token-matched neutral histories are synthetic, not representative author conversations. '
                               'Instructions/oracle are experiments, not production fixes.' if args.phase == 'mechanism' else
                               'Seven previously captured questions, not fresh independent manuscripts. The question field anchors the Collins controls; arms.json identifies each cross-question input. No production fix is certified.')}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        # All arms and counts are frozen before launching inference.
        (args.output / 'arms.json').write_text(json.dumps(arms, ensure_ascii=False, indent=2)+'\n')
        run_server(args.output, command, sock, arms)
        props = json.loads((args.output / 'props.json').read_text())
        if props['default_generation_settings']['n_ctx'] != 8192:
            raise RuntimeError('Actual slot mismatch; results must not be interpreted')


if __name__ == '__main__':
    main()
