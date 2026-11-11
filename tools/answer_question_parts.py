#!/usr/bin/env python3
"""Per-part reranking arm: does scoring each part of a multi-part question
separately put the designated units first? Diagnostic only; no production change.

Pass 6 of the verified pipeline (journal/answer-cycle2_pipeline_live.md) chose,
for the two-part fortunes question, Mr. Bingley's inheritance (right) and Miss
de Bourgh's fortune (wrong: the sisters' unit sat at rank 7), and the composer
attributed the wrong fortune to the sisters. The relation error is downstream
of selection: a unit that answers no part of the question was handed to the
composer as evidence. This arm takes the candidate pools the app scored in a
live run (ANSWER_PIPELINE_SCORES in its trace), splits each question into its
parts in software, and scores every candidate against the whole question and
against each part with the same reranker, prompt, clip and scoring path as the
C client (src/llm/rerank.c). The whole-question scores are compared with the
app's logged scores as the harness parity check.
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
from answer_pipeline_review import RECEIPT, parse_trace  # noqa: E402
from answer_rerank_arm import rerank_prompt  # noqa: E402
from answer_rerank_runtime import GGUF, GGUF_SHA, llama_score, token_id  # noqa: E402
from answer_unit_extraction import session  # noqa: E402
from memory_causal_parity import ROOT, sha  # noqa: E402

FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'
RERANK_CLIP_BYTES = 2000  # src/llm/rerank.c
SPANS = 2  # ANSWER_SPANS
WH = r'(?:what|which|why|who|whom|whose|how|when|where)\b'
# A conjunction that starts a new question part: ", and what ..." / "; and why ..." / " and how ...".
CONJUNCTION = re.compile(r'[,;]?\s+(?:and|or|but)\s+(?=' + WH + ')', re.I)
# Sentence ends; an honorific's period is not one ("Mr. Bennet's estate").
SENTENCE_END = re.compile(r'(?<!\bMr\.)(?<!\bMrs\.)(?<!\bMs\.)(?<!\bDr\.)(?<!\bSt\.)(?<=[.?!])\s+')


def question_parts(question):
    """The question sentences of the request, each conjunction-joined wh-part on its own, each ending in '?'.
    A request with no question sentence is one part, unchanged. Software only: no model."""
    sentences = [s.strip() for s in SENTENCE_END.split(question.strip()) if s.strip()]
    asked = [s for s in sentences if s.endswith('?')]
    if not asked:
        return [question.strip()]
    parts = []
    for s in asked:
        for piece in CONJUNCTION.split(s):
            piece = piece.strip().rstrip('?,;').strip()
            if piece:
                parts.append(piece + '?')
    return parts


def question_context(question):
    """The request's non-question sentences, in order: the referent a bare part loses ("My two drafts disagree about
    how many miles Elizabeth walks to Netherfield." before "Which figure does the current manuscript give?")."""
    sentences = [s.strip() for s in SENTENCE_END.split(question.strip()) if s.strip()]
    return ' '.join(s for s in sentences if not s.endswith('?'))


def clip_utf8(text, limit=RERANK_CLIP_BYTES):
    """The first `limit` bytes of the UTF-8 encoding, cut at a character boundary (utf8_clip)."""
    raw = text.encode('utf-8')
    if len(raw) <= limit:
        return text
    return raw[:limit].decode('utf-8', errors='ignore')


def case_pools(fixture, messages, traces):
    """Per fixture case: the scored candidate pool (unit id prefix, logged score) from the live run, paired to the
    case's reply by the chosen unit in the receipt, traces consumed in reply order (as the review tool does)."""
    users = [m for m in messages if m['role'] == 'user']
    by_seq = {}
    unused = list(traces)
    for m in sorted(messages, key=lambda m: m['seq']):
        if m['role'] != 'assistant':
            continue
        r = RECEIPT.search(m.get('memory_sources') or '')
        unit = r[1] if r and r[1] else None
        if not unit:
            continue
        for i, (t, sc) in enumerate(unused):
            if t.get('top', '').startswith(unit[:12]):
                by_seq[m['seq']] = (t, sc); del unused[i]; break
    pools = {}
    for case in fixture['cases']:
        q = [m for m in users if m['content'] == case['question']]
        if len(q) != 1:
            continue
        trace, scores = by_seq.get(q[0]['seq'] + 1, ({}, []))
        if scores:
            pools[case['id']] = {'trace': trace, 'scores': scores}
    return pools


def designated_ids(case, units):
    out = {}
    for d in case['designated']:
        hits = [u for u in units if u['document_name'] == d['document'] and d['passage'] in u['text']]
        if len(hits) == 1:
            out[hits[0]['unit_id'][:12]] = d['passage']
    return out


def ranks(scored):
    """{prefix: rank} with ties broken by pool order, 1-based."""
    order = sorted(range(len(scored)), key=lambda i: -scored[i][1])
    return {scored[i][0]: r + 1 for r, i in enumerate(order)}


def slots(parts_scored, full_scored, nslots=SPANS):
    """The units the two slots would hold: one per part (top of that part's ranking, distinct units), the rest
    filled from the whole-question ranking. With one part this is today's selection."""
    chosen = []
    for scored in parts_scored:
        for prefix, _ in sorted(scored, key=lambda s: -s[1]):
            if prefix not in chosen:
                chosen.append(prefix); break
        if len(chosen) == nslots:
            break
    for prefix, _ in sorted(full_scored, key=lambda s: -s[1]):
        if len(chosen) == nslots:
            break
        if prefix not in chosen:
            chosen.append(prefix)
    return chosen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--product', type=Path, required=True, help='a live run directory: app.log, messages.json, units.json')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    messages = json.loads((args.product / 'messages.json').read_text())
    units = json.loads((args.product / 'units.json').read_text())
    by_prefix = {u['unit_id'][:12]: u for u in units}
    traces = parse_trace((args.product / 'app.log').read_text(errors='replace'))
    pools = case_pools(fixture, messages, traces)
    if sha(GGUF) != GGUF_SHA:
        raise ValueError('Pinned reranker GGUF mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (FIXTURE, Path(__file__)):
        shutil.copy2(source, inputs / source.name)
    binary = ROOT / 'build/llama/bin/llama-server'
    cases = [c for c in fixture['cases'] if c['id'] in pools and c['designated']]
    plan = []
    for c in cases:
        parts = question_parts(c['question'])
        queries = [('full', c['question'])] + [(f'part{i + 1}', p) for i, p in enumerate(parts)]
        context = question_context(c['question'])
        if context and parts != [c['question']]:
            queries += [(f'cpart{i + 1}', f'{context} {p}') for i, p in enumerate(parts)]
        plan.append((c, queries))
    npairs = sum(len(pools[c['id']]['scores']) * len(qs) for c, qs in plan)
    results = {'cases': [], 'pairs': npairs}
    with private_socket() as sock:
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '-np', '1', '-c', '2048', '-b', '2048', '-ub', '2048', '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'command': command, 'gguf_sha256': GGUF_SHA, 'binary_sha256': sha(binary), 'product': str(args.product),
                    'product_sha256': {n: sha(args.product / n) for n in ('app.log', 'messages.json', 'units.json')},
                    'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()}, 'pairs': npairs, 'clip_bytes': RERANK_CLIP_BYTES,
                    'limits': 'Candidate pools are the ones the app retrieved for the whole question; per-part retrieval would '
                              'change the pools. Ranks are among those candidates only. One live run, one reranker.'}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with session(args.output, command, sock):
            yes, no = token_id(sock, 'yes'), token_id(sock, 'no')
            t0 = time.monotonic()
            for c, queries in plan:
                pool = pools[c['id']]['scores']
                designated = designated_ids(c, units)
                scored = {}
                for name, query in queries:
                    scored[name] = [(prefix, llama_score(sock, rerank_prompt(query, clip_utf8(by_prefix[prefix]['text'])), yes, no)[0])
                                    for prefix, _ in pool]
                logged = ranks(pool)
                row = {'id': c['id'], 'question': c['question'], 'parts': [q for n, q in queries if n.startswith('part')],
                       'context_parts': [q for n, q in queries if n.startswith('cpart')], 'pool': len(pool),
                       'logged_vs_offline_max_abs_diff': max(abs(ls - s) for (_, ls), (_, s) in zip(pool, scored['full'])),
                       'queries': {}, 'designated': []}
                for name, _ in queries:
                    r = ranks(scored[name])
                    top = min(r, key=r.get)
                    row['queries'][name] = {'top': top, 'top_document': by_prefix[top]['document_name'], 'top_designated': top in designated,
                                            'top_text': re.sub(r'\s+', ' ', by_prefix[top]['text'])[:160],
                                            'scores': {p: round(s, 4) for p, s in scored[name]}}
                for prefix, passage in designated.items():
                    row['designated'].append({'unit': prefix, 'passage': passage, 'logged_rank': logged.get(prefix),
                                              **{f'{name}_rank': ranks(scored[name]).get(prefix) for name, _ in queries}})
                part_names = [n for n, _ in queries if n.startswith('part')]
                cpart_names = [n for n, _ in queries if n.startswith('cpart')] or part_names
                row['slots_today'] = slots([], scored['full'])
                row['slots_by_part'] = slots([scored[n] for n in part_names], scored['full'])
                row['slots_by_context_part'] = slots([scored[n] for n in cpart_names], scored['full'])
                row['slots_today_designated'] = sum(p in designated for p in row['slots_today'])
                row['slots_by_part_designated'] = sum(p in designated for p in row['slots_by_part'])
                row['slots_by_context_part_designated'] = sum(p in designated for p in row['slots_by_context_part'])
                results['cases'].append(row)
                (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
                names = [n for n, _ in queries if n != 'full']
                print(f"{c['id']:26s} parts={len(part_names)} pool={len(pool)} parity={row['logged_vs_offline_max_abs_diff']:.4f} "
                      f"designated(logged,full,{','.join(names)})="
                      + str([(d['logged_rank'], d['full_rank'], *[d[f'{n}_rank'] for n in names]) for d in row['designated']])
                      + f" slots designated today={row['slots_today_designated']} by_part={row['slots_by_part_designated']} "
                        f"by_context_part={row['slots_by_context_part_designated']}", flush=True)
            results['seconds_per_pair'] = round((time.monotonic() - t0) / max(npairs, 1), 3)
    results['summary'] = {
        'cases': len(results['cases']),
        'parity_max_abs_diff': max(r['logged_vs_offline_max_abs_diff'] for r in results['cases']),
        'designated_units': sum(len(r['designated']) for r in results['cases']),
        'designated_first_full': sum(d['full_rank'] == 1 for r in results['cases'] for d in r['designated']),
        'designated_first_any_part': sum(any(d[f'part{i + 1}_rank'] == 1 for i in range(len(r['parts']))) for r in results['cases'] for d in r['designated']),
        'slots_designated_today': sum(r['slots_today_designated'] for r in results['cases']),
        'slots_designated_by_part': sum(r['slots_by_part_designated'] for r in results['cases']),
        'slots_designated_by_context_part': sum(r['slots_by_context_part_designated'] for r in results['cases']),
        'seconds_per_pair': results['seconds_per_pair']}
    (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(results['summary'], indent=2))


if __name__ == '__main__':
    main()
