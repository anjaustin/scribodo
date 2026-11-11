#!/usr/bin/env python3
"""Coarse-to-fine reranking arm: sentence windows of the top units, scored
against the question or its part. Diagnostic only; no production change.

The per-part arm (tools/answer_question_parts.py) found the reranker saturated
at unit level: for "What fortune do Mr. Bingley's sisters have?" five units that
mention Bingley or a fortune score 0.97 to 0.99 and the stating unit is fifth.
This arm takes each query's top units from that arm's results, cuts them into
sentences, scores every window of W consecutive sentences with the same
reranker, prompt and scoring path as the C client, and reports where the window
holding the designated passage ranks among all windows, what the top window is,
and which units the two slots would hold if each part took its top window's unit.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answer_bedrock_probe import private_socket  # noqa: E402
from answer_question_parts import clip_utf8, ranks  # noqa: E402
from answer_rerank_arm import rerank_prompt  # noqa: E402
from answer_rerank_runtime import GGUF, GGUF_SHA, llama_score, token_id  # noqa: E402
from answer_unit_extraction import session  # noqa: E402
from memory_causal_parity import ROOT, sha  # noqa: E402

# A sentence ends at . ? or !, optionally followed by a closing quote; an honorific's period is not an end.
SENTENCE_END = re.compile(r'(?<!\bMr\.)(?<!\bMrs\.)(?<!\bMs\.)(?<!\bDr\.)(?<!\bSt\.)(?:(?<=[.?!])|(?<=[.?!][”"’\']))\s+')


def sentences(text):
    """Whitespace-collapsed sentences of a unit, in order."""
    flat = re.sub(r'\s+', ' ', text).strip()
    return [s for s in SENTENCE_END.split(flat) if s]


def windows(text, width, stride=1):
    """(start sentence index, window text) for every window of `width` consecutive sentences; a unit with fewer
    sentences is one window."""
    sents = sentences(text)
    if len(sents) <= width:
        return [(0, ' '.join(sents))] if sents else []
    return [(i, ' '.join(sents[i:i + width])) for i in range(0, len(sents) - width + 1, stride)]


def query_text(row, name):
    if name == 'full':
        return row['question']
    if name.startswith('cpart'):
        return row['context_parts'][int(name[5:]) - 1]
    return row['parts'][int(name[4:]) - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True, help='results.json of tools/answer_question_parts.py')
    parser.add_argument('--product', type=Path, required=True, help='the live run directory those results came from')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--top', type=int, default=8, help='units per query taken from the unit ranking')
    parser.add_argument('--width', type=int, default=3, help='sentences per window')
    parser.add_argument('--stride', type=int, default=1)
    args = parser.parse_args()
    parts_results = json.loads(args.results.read_text())
    units = json.loads((args.product / 'units.json').read_text())
    by_prefix = {u['unit_id'][:12]: u for u in units}
    if sha(GGUF) != GGUF_SHA:
        raise ValueError('Pinned reranker GGUF mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (args.results, Path(__file__)):
        shutil.copy2(source, inputs / source.name)
    binary = ROOT / 'build/llama/bin/llama-server'
    plan = []
    for row in parts_results['cases']:
        for name, q in row['queries'].items():
            top = [p for p, _ in sorted(q['scores'].items(), key=lambda kv: -kv[1])[:args.top]]
            wins = [(p, start, text) for p in top for start, text in windows(by_prefix[p]['text'], args.width, args.stride)]
            plan.append((row, name, query_text(row, name), top, wins))
    npairs = sum(len(w) for *_, w in plan)
    results = {'cases': {}, 'pairs': npairs, 'top': args.top, 'width': args.width, 'stride': args.stride}
    with private_socket() as sock:
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '-np', '1', '-c', '2048', '-b', '2048', '-ub', '2048', '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'command': command, 'gguf_sha256': GGUF_SHA, 'binary_sha256': sha(binary), 'product': str(args.product),
                    'units_sha256': sha(args.product / 'units.json'), 'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()},
                    'pairs': npairs, 'top': args.top, 'width': args.width, 'stride': args.stride,
                    'limits': 'Windows are cut by a sentence regex, not by the manuscript\'s own structure; the top units come '
                              'from one unit ranking of one live run\'s candidate pool. Ranks are among those windows only.'}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with session(args.output, command, sock):
            yes, no = token_id(sock, 'yes'), token_id(sock, 'no')
            t0 = time.monotonic()
            for row, name, query, top, wins in plan:
                scored, failures = [], 0
                for p, start, text in wins:
                    try:
                        score = llama_score(sock, rerank_prompt(query, clip_utf8(text)), yes, no)[0]
                    except ValueError:  # neither yes nor no in the top probabilities: the C client counts a failure
                        score, failures = 0.0, failures + 1
                    scored.append(((p, start), score))
                r = ranks(scored)
                texts = {(p, start): text for p, start, text in wins}
                designated = {d['unit']: d['passage'] for d in row['designated']}
                best = min(r, key=r.get) if r else None
                out = {'query': query, 'top_units': top, 'windows': len(wins), 'score_failures': failures,
                       'top_window': {'unit': best[0], 'start': best[1], 'document': by_prefix[best[0]]['document_name'],
                                      'score': round(dict(scored)[best], 4), 'designated_unit': best[0] in designated,
                                      'holds_passage': any(p in texts[best] for p in designated.values()),
                                      'text': texts[best]} if best else None,
                       'designated': []}
                for unit, passage in designated.items():
                    holding = [k for k in r if k[0] == unit and passage in texts[k]]
                    out['designated'].append({'unit': unit, 'passage': passage, 'in_top_units': unit in top,
                                              'windows_holding_passage': len(holding),
                                              'best_rank': min(r[k] for k in holding) if holding else None,
                                              'best_score': round(max(dict(scored)[k] for k in holding), 4) if holding else None,
                                              'unit_best_rank': min((r[k] for k in r if k[0] == unit), default=None)})
                out['ranking'] = [{'unit': k[0], 'start': k[1], 'score': round(s, 4), 'text': texts[k][:200]}
                                  for k, s in sorted(scored, key=lambda ks: -ks[1])[:10]]
                results['cases'].setdefault(row['id'], {})[name] = out
                (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
                tw = out['top_window']
                print(f"{row['id']:26s} {name:6s} windows={len(wins):3d} designated(in_top,best_rank,unit_best)="
                      + str([(d['in_top_units'], d['best_rank'], d['unit_best_rank']) for d in out['designated']])
                      + f" top={'designated' if tw and tw['holds_passage'] else 'other'}:{tw['document'] if tw else None}:"
                      + (re.sub(r'\s+', ' ', tw['text'])[:70] if tw else ''), flush=True)
            results['seconds_per_pair'] = round((time.monotonic() - t0) / max(npairs, 1), 3)
    summary = {'queries': sum(len(v) for v in results['cases'].values()), 'pairs': npairs,
               'designated_windows': sum(len(o['designated']) for v in results['cases'].values() for o in v.values()),
               'designated_window_first': sum(d['best_rank'] == 1 for v in results['cases'].values() for o in v.values() for d in o['designated']),
               'designated_window_top3': sum(d['best_rank'] is not None and d['best_rank'] <= 3 for v in results['cases'].values() for o in v.values() for d in o['designated']),
               'seconds_per_pair': results['seconds_per_pair']}
    results['summary'] = summary
    (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
