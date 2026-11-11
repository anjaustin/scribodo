#!/usr/bin/env python3
"""Admission arm: give each tool the job it measured well at. Diagnostic only;
no production change.

Cycle 3 measured that the embedder tells whose attribute a passage is about
(the sisters' unit first for the sisters' part, Miss de Bourgh's 54th) while
the cross-encoder finds a stating passage inside an admitted set but cannot
make that distinction (it lifted Miss de Bourgh's unit from 42nd to 2nd). Today
the reranker judges a pool fed on equal terms by the embedder and lexical
overlap, and the two slots are its top two.

This arm, over the document units of a live run and the boundary fixture:
- splits each question into parts in software, each carrying the request's
  context sentence (tools/answer_question_parts.py);
- admits a pool per part from the embedder alone, or the embedder plus lexical
  overlap, at sizes 8, 16 and 32;
- orders the pool with the reranker, or not at all;
- assigns the two slots as a maximum-score matching over parts and distinct
  units, instead of the greedy top two;
and scores every variant as designated units in slots and designated units in
the pool, beside today's shape (whole question, embedder 32 plus lexical 32,
greedy top two). The embedder, reranker, prompts and clips are the pinned ones.
"""
import argparse
from itertools import product
import json
from pathlib import Path
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answer_bedrock_probe import private_socket  # noqa: E402
from answer_question_parts import clip_utf8, question_context, question_parts  # noqa: E402
from answer_rerank_arm import lexical_score, rerank_prompt  # noqa: E402
from answer_rerank_runtime import GGUF, GGUF_SHA, llama_score, token_id  # noqa: E402
from answer_unit_extraction import EMBED_GGUF, EMBED_SHA, embed, rank, session  # noqa: E402
from memory_causal_parity import ROOT, sha  # noqa: E402

FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'
SIZES = (8, 16, 32)
SLOTS = 2


def context_parts(question):
    parts = question_parts(question)
    context = question_context(question)
    return [f'{context} {p}' if context and parts != [question] else p for p in parts]


def top_k(scores, k):
    """Indices of the k highest scores, ties by index."""
    return [i for i, _ in sorted(enumerate(scores), key=lambda s: (-s[1], s[0]))[:k]]


def union_pool(embed_scores, lexical_scores, k, with_lexical):
    pool = top_k(embed_scores, k)
    if with_lexical:
        pool += [i for i in top_k(lexical_scores, k) if i not in pool]
    return pool


def matching(part_pools, score, nslots=SLOTS):
    """Slots as a maximum-score assignment: one distinct unit per part (parts beyond the slot count are dropped),
    then remaining slots filled greedily from the first part's pool. `score(part_index, unit)` is the ordering
    score. With one part this is the greedy top two of its pool."""
    parts = list(range(min(len(part_pools), nslots)))
    best, best_total = None, None
    for choice in product(*(part_pools[p] for p in parts)):
        if len(set(choice)) < len(choice):
            continue
        total = sum(score(p, u) for p, u in zip(parts, choice))
        if best_total is None or total > best_total:
            best, best_total = list(choice), total
    chosen = best or []
    for u in sorted(part_pools[0], key=lambda u: -score(0, u)):
        if len(chosen) >= nslots:
            break
        if u not in chosen:
            chosen.append(u)
    return chosen


def greedy(pool, score, nslots=SLOTS):
    return sorted(pool, key=lambda u: -score(u))[:nslots]


RRF_K = 60  # the reciprocal rank fusion constant of Cormack, Clarke and Buettcher (2009); a convention, not tuned here


def rrf(pool, *rankings, k=RRF_K):
    """Reciprocal rank fusion over the pool: each ranking is a score function; a unit's fused score is the sum of
    1 / (k + rank) over the rankings, ranks within the pool, ties by index."""
    fused = {u: 0.0 for u in pool}
    for score in rankings:
        for r, u in enumerate(sorted(pool, key=lambda u: (-score(u), u)), 1):
            fused[u] += 1.0 / (k + r)
    return fused


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--product', type=Path, required=True, help='a live run directory with units.json')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    units = [u for u in json.load(open(args.product / 'units.json')) if u['source_kind'] == 'document']
    cases = [c for c in fixture['cases'] if c['designated']]
    for sha_path, expected in ((EMBED_GGUF, EMBED_SHA), (GGUF, GGUF_SHA)):
        if sha(sha_path) != expected:
            raise ValueError(f'Pinned model mismatch: {sha_path}')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (FIXTURE, Path(__file__)):
        shutil.copy2(source, inputs / source.name)
    binary = ROOT / 'build/llama/bin/llama-server'
    plan = []
    for c in cases:
        designated = set()
        for d in c['designated']:
            hits = [i for i, u in enumerate(units) if u['document_name'] == d['document'] and d['passage'] in u['text']]
            if len(hits) == 1:
                designated.add(hits[0])
        queries = {'whole': c['question'], **{f'part{i + 1}': p for i, p in enumerate(context_parts(c['question']))}}
        plan.append((c, queries, designated))
    manifest = {'embedder_sha256': EMBED_SHA, 'reranker_sha256': GGUF_SHA, 'binary_sha256': sha(binary), 'product': str(args.product),
                'units_sha256': sha(args.product / 'units.json'), 'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()},
                'units': len(units), 'cases': [c['id'] for c in cases], 'sizes': SIZES,
                'limits': 'Document units only (production candidates are manuscript-only since pass 2). The lexical feeder is '
                          'content-word overlap, a stand-in for production lexical retrieval, not it. One live run, pinned models.'}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    # Stage 1: the embedder over every unit and every query.
    timings = {}
    with private_socket() as sock:
        command = [str(binary), '-m', str(EMBED_GGUF), '-ngl', '99', '-fa', 'off', '--embedding', '--pooling', 'last',
                   '-np', '1', '-c', '2304', '-b', '2304', '-ub', '2304', '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        embed_dir = args.output / 'embed'; embed_dir.mkdir()
        t0 = time.monotonic()
        with session(embed_dir, command, sock):
            vectors = []
            for i in range(0, len(units), 4):
                vectors += embed(sock, [u['text'] for u in units[i:i + 4]], query=False)
            query_vectors = {(c['id'], name): embed(sock, [q], query=True)[0] for c, queries, _ in plan for name, q in queries.items()}
        timings['embedder_seconds'] = round(time.monotonic() - t0, 1)
    embed_scores = {}
    for key, qv in query_vectors.items():
        ranked = rank(qv, vectors)
        scores = [0.0] * len(units)
        for i, s in ranked:
            scores[i] = s
        embed_scores[key] = scores
    lexical_scores = {(c['id'], name): [lexical_score(q, u['text']) for u in units] for c, queries, _ in plan for name, q in queries.items()}
    # Stage 2: the reranker over every (query, unit) pair any variant can need: the union of the largest pools.
    pairs = {}
    for c, queries, _ in plan:
        for name in queries:
            key = (c['id'], name)
            for i in union_pool(embed_scores[key], lexical_scores[key], max(SIZES), True):
                pairs[(key, i)] = None
    with private_socket() as sock:
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '-np', '1', '-c', '2048', '-b', '2048', '-ub', '2048', '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        rerank_dir = args.output / 'rerank'; rerank_dir.mkdir()
        t0 = time.monotonic(); failures = 0
        with session(rerank_dir, command, sock):
            yes, no = token_id(sock, 'yes'), token_id(sock, 'no')
            for (key, i) in list(pairs):
                c = next(c for c, _, _ in plan if c['id'] == key[0])
                query = {**{'whole': c['question']}, **{f'part{j + 1}': p for j, p in enumerate(context_parts(c['question']))}}[key[1]]
                try:
                    pairs[(key, i)] = llama_score(sock, rerank_prompt(query, clip_utf8(units[i]['text'])), yes, no)[0]
                except ValueError:
                    pairs[(key, i)] = 0.0; failures += 1
        timings['reranker_seconds'] = round(time.monotonic() - t0, 1); timings['reranker_pairs'] = len(pairs); timings['score_failures'] = failures
    # Variants.
    results = {'timings': timings, 'cases': [], 'variants': {}}
    for c, queries, designated in plan:
        cid = c['id']
        part_names = [n for n in queries if n != 'whole']
        row = {'id': cid, 'parts': [queries[n] for n in part_names], 'designated': sorted(designated), 'variants': {}}

        def record(name, pools, slots):
            pooled = set().union(*pools) if pools else set()
            row['variants'][name] = {'slots': slots, 'slots_designated': sum(u in designated for u in slots),
                                     'pool_designated': sum(u in pooled for u in designated), 'pool_size': len(pooled),
                                     'slot_texts': [units[u]['text'][:80] for u in slots]}
            v = results['variants'].setdefault(name, {'slots_designated': 0, 'pool_designated': 0, 'designated': 0, 'cases_all_designated': 0})
            v['slots_designated'] += row['variants'][name]['slots_designated']
            v['pool_designated'] += row['variants'][name]['pool_designated']
            v['designated'] += len(designated)
            v['cases_all_designated'] += int(designated and designated <= set(slots))
        whole = (cid, 'whole')
        # Today's shape: whole question, embedder 32 plus lexical 32, reranker, greedy top two.
        pool = union_pool(embed_scores[whole], lexical_scores[whole], 32, True)
        record('today_whole_union32_rerank_greedy', [pool], greedy(pool, lambda u: pairs[(whole, u)]))
        for k in SIZES:
            for with_lexical in (False, True):
                feed = 'union' if with_lexical else 'embed'
                pool = union_pool(embed_scores[whole], lexical_scores[whole], k, with_lexical)
                record(f'whole_{feed}{k}_rerank_greedy', [pool], greedy(pool, lambda u: pairs[(whole, u)]))
                part_pools = [union_pool(embed_scores[(cid, n)], lexical_scores[(cid, n)], k, with_lexical) for n in part_names]
                record(f'parts_{feed}{k}_rerank_matching', part_pools,
                       matching(part_pools, lambda p, u: pairs[((cid, part_names[p]), u)]))
                record(f'parts_{feed}{k}_embed_matching', part_pools,
                       matching(part_pools, lambda p, u: embed_scores[(cid, part_names[p])][u]))
                fused = [rrf(pool_p, lambda u, n=n: embed_scores[(cid, n)][u], lambda u, n=n: pairs[((cid, n), u)])
                         for pool_p, n in zip(part_pools, part_names)]
                record(f'parts_{feed}{k}_rrf_matching', part_pools, matching(part_pools, lambda p, u: fused[p][u]))
        results['cases'].append(row)
    results['scores'] = {f'{cid}|{name}': {'embed': {str(u): round(embed_scores[(cid, name)][u], 4) for u in union_pool(embed_scores[(cid, name)], lexical_scores[(cid, name)], max(SIZES), True)},
                                           'lexical': {str(u): round(lexical_scores[(cid, name)][u], 4) for u in union_pool(embed_scores[(cid, name)], lexical_scores[(cid, name)], max(SIZES), True)},
                                           'rerank': {str(u): round(pairs[((cid, name), u)], 4) for u in union_pool(embed_scores[(cid, name)], lexical_scores[(cid, name)], max(SIZES), True)}}
                         for c, queries, _ in plan for cid in [c['id']] for name in queries}
    (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(timings))
    print(f"{'variant':40s} slots_designated pool_designated cases_complete (designated units: {results['variants']['today_whole_union32_rerank_greedy']['designated']}, cases: {len(plan)})")
    for name, v in results['variants'].items():
        print(f"{name:40s} {v['slots_designated']:16d} {v['pool_designated']:15d} {v['cases_all_designated']:14d}")
    print()
    for row in results['cases']:
        print(row['id'], {n: v['slots_designated'] for n, v in row['variants'].items() if n.startswith(('today', 'parts_')) and ('rerank' in n or 'rrf' in n)})


if __name__ == '__main__':
    main()
