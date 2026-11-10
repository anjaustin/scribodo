#!/usr/bin/env python3
"""Reranker on llama.cpp against PyTorch: score parity and speed; diagnostic only.

The in-app runtime is llama-server, so the Qwen3-Reranker GGUF (converted from
the pinned Hugging Face revision with the pinned llama.cpp converter, BF16) is
scored through /completion: one predicted token, the top probabilities at that
position, P(yes) renormalized against P(no). The same pairs are scored by the
PyTorch float32 reference used in build/rk1 and rk2. Reports the maximum score
difference, rank agreement and wall time per pair for both runtimes.
"""
import argparse
import json
from pathlib import Path
import random
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answer_bedrock_probe import call, private_socket  # noqa: E402
from answer_ranker_instruction import pools_from_record  # noqa: E402
from answer_rerank_arm import RERANKER, RERANKER_REVISION, Reranker, order, rerank_prompt  # noqa: E402
from answer_unit_extraction import session  # noqa: E402
from memory_causal_parity import ROOT, sha  # noqa: E402

FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'
GGUF = ROOT / 'build/rerank/Qwen3-Reranker-0.6B-bf16.gguf'
GGUF_SHA = '5a6bf98df1ba7b7a957bbcfc14e74a73b4c314812c2224fe048ea942db4e53f1'
SAMPLE_UNITS = 40


def token_id(sock, text):
    ids = call(sock, '/tokenize', {'content': text, 'add_special': False})['tokens']
    if len(ids) != 1:
        raise ValueError(f'{text!r} is not a single token: {ids}')
    return ids[0]


def llama_score(sock, prompt, yes, no, timeout=120):
    """P(yes) over {yes, no} from the top probabilities of the first predicted token."""
    body = {'prompt': prompt, 'n_predict': 1, 'n_probs': 20, 'temperature': 0, 'top_k': 1, 'samplers': ['top_k'],
            'cache_prompt': False, 'post_sampling_probs': False}
    response = call(sock, '/completion', body, timeout=timeout)
    probs = response['completion_probabilities'][0]
    top = probs.get('top_logprobs') or probs.get('top_probs') or []
    p = {}
    for row in top:
        if 'logprob' in row:
            p[row['id']] = 2.718281828459045 ** row['logprob']
        else:
            p[row['id']] = row['prob']
    py, pn = p.get(yes, 0.0), p.get(no, 0.0)
    if py + pn == 0:
        raise ValueError('Neither yes nor no in the top probabilities')
    return py / (py + pn), {'yes_in_top': yes in p, 'no_in_top': no in p, 'prompt_tokens': response.get('tokens_evaluated')}


def pairs_from(record, units, questions, designated, seed=42):
    pairs = []
    pools = pools_from_record(record)
    for cid, scopes in pools.items():
        for scope, pool in scopes.items():
            for s in pool:
                pairs.append({'kind': 'span', 'case': cid, 'scope': scope, 'expected': s[4], 'question': questions[cid], 'text': s[1]})
    by_id = {u['unit_id']: u for u in units}
    for cid, ids in designated.items():
        for uid in sorted(ids):
            pairs.append({'kind': 'designated_unit', 'case': cid, 'expected': True, 'question': questions[cid], 'text': by_id[uid]['text']})
    rng = random.Random(seed)
    sample = rng.sample([u for u in units if u['source_kind'] == 'document'], SAMPLE_UNITS)
    cases = [c for c in questions if designated.get(c)]
    for i, u in enumerate(sample):
        cid = cases[i % len(cases)]
        pairs.append({'kind': 'sample_unit', 'case': cid, 'expected': u['unit_id'] in designated[cid], 'question': questions[cid], 'text': u['text']})
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', type=Path, required=True)
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device', default='mps')
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    questions = {c['id']: c['question'] for c in fixture['cases']}
    record = json.loads(args.record.read_text())
    units = json.loads((args.product / 'units.json').read_text())
    designated = {c['id']: {uid for uid, flag in c['expected_answers'].items() if flag}
                  for c in json.loads((args.record.parent / 'plan.json').read_text())}
    if sha(GGUF) != GGUF_SHA:
        raise ValueError('Pinned reranker GGUF mismatch')
    pairs = pairs_from(record, units, questions, designated)
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (FIXTURE, args.record, args.record.parent / 'plan.json', Path(__file__)):
        shutil.copy2(source, inputs / source.name)
    binary = ROOT / 'build/llama/bin/llama-server'
    prompts = [rerank_prompt(p['question'], p['text']) for p in pairs]
    with private_socket() as sock:
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '-np', '1', '-c', '2048', '-b', '2048', '-ub', '2048', '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'command': command, 'gguf_sha256': GGUF_SHA, 'binary_sha256': sha(binary), 'reranker': RERANKER,
                    'reranker_revision': RERANKER_REVISION, 'pairs': len(pairs),
                    'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()},
                    'limits': 'Parity of one scoring path (top probabilities at the first predicted token) against the PyTorch '
                              'float32 reference on a fixed pair set. Not a quality claim about the reranker itself.'}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with session(args.output, command, sock):
            yes, no = token_id(sock, 'yes'), token_id(sock, 'no')
            manifest['yes_token'], manifest['no_token'] = yes, no
            t0 = time.monotonic()
            llama = [llama_score(sock, prompt, yes, no) for prompt in prompts]
            llama_seconds = time.monotonic() - t0
    from huggingface_hub import snapshot_download
    snapshot = Path(snapshot_download(RERANKER, revision=RERANKER_REVISION))
    reranker = Reranker(snapshot, args.device)
    t0 = time.monotonic()
    torch_scores = reranker.scores(prompts)
    torch_seconds = time.monotonic() - t0
    rows = []
    for pair, (ls, meta), ts in zip(pairs, llama, torch_scores):
        rows.append({**{k: pair[k] for k in ('kind', 'case', 'expected')}, 'text': pair['text'][:80], 'llama': round(ls, 6),
                     'torch': round(ts, 6), 'diff': round(ls - ts, 6), **meta})
    diffs = [abs(r['diff']) for r in rows]
    llama_order = [i for i, _ in order([r['llama'] for r in rows])]
    torch_order = [i for i, _ in order([r['torch'] for r in rows])]
    same_top10 = len(set(llama_order[:10]) & set(torch_order[:10]))
    # Rank agreement per question among its pairs: does each runtime put the same pair first?
    by_case = {}
    for i, r in enumerate(rows):
        by_case.setdefault(r['case'], []).append(i)
    first_agree = sum(max(idx, key=lambda i: rows[i]['llama']) == max(idx, key=lambda i: rows[i]['torch']) for idx in by_case.values())
    # Decision agreement: expected pairs scored above 0.5 by both, non-expected below.
    agree = sum((r['llama'] >= .5) == (r['torch'] >= .5) for r in rows)
    summary = {'pairs': len(rows), 'max_abs_diff': round(max(diffs), 6), 'mean_abs_diff': round(sum(diffs) / len(diffs), 6),
               'pairs_within_0_01': sum(d <= .01 for d in diffs), 'pairs_within_0_05': sum(d <= .05 for d in diffs),
               'threshold_0_5_agreement': agree, 'per_question_first_place_agreement': f'{first_agree}/{len(by_case)}',
               'top10_overlap': same_top10, 'yes_and_no_in_top_probs': sum(r['yes_in_top'] and r['no_in_top'] for r in rows),
               'llama_seconds_per_pair': round(llama_seconds / len(rows), 3), 'torch_seconds_per_pair': round(torch_seconds / len(rows), 3),
               'llama_seconds': round(llama_seconds, 1), 'torch_seconds': round(torch_seconds, 1)}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (args.output / 'parity.json').write_text(json.dumps({'summary': summary, 'rows': rows}, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
