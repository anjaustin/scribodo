#!/usr/bin/env python3
"""Ranker instruction comparison on the production embedder; deterministic, no generative model.

The ranked-span arm (build/ux4) used the production query instruction, which is
written for statements, and ranked the stating span first in 4 of 5 realistic
pools while demoting every short span. This tool re-ranks the same verified
span pools under several fixed query instructions, and also ranks the
designated unit among every exported unit of the run, which is the retrieval
side the same instruction governs. Spans and units are embedded without an
instruction, as production does; only the query side changes. No production
instruction, model or flag changes.
"""
import argparse
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answer_bedrock_probe import private_socket  # noqa: E402
from answer_unit_extraction import EMBED_GGUF, EMBED_SHA, QUERY_INSTRUCTION, embed, rank, session  # noqa: E402
from memory_causal_parity import ROOT, sha  # noqa: E402

FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'

# Fixed before the run. 'production' is src/llm/semantic.c's k_query_instruction, byte for byte.
INSTRUCTIONS = {
    'production': QUERY_INSTRUCTION,
    'question': 'Instruct: Given a question about a work of fiction, retrieve the passage from the work that states the answer\nQuery: ',
    'qwen_default': 'Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery: ',
    'none': '',
}


def pools_from_record(record):
    """Per case: the verified spans (label, span, unit_id, added, expected_answers), realistic and added pools."""
    pools = {}
    for entry in record['cases']:
        spans = [(u['label'], u['span'], u['unit_id'], u['added'], u['expected_answers']) for u in entry['units'] if u['span']]
        realistic = [s for s in spans if not s[3]]
        pools[entry['id']] = {'realistic': realistic, 'added': spans if any(s[3] for s in spans) else []}
    return pools


def pool_counts(pools, designated):
    """Predeclaration counts from the data, not by hand (REDTEAM-020 R4)."""
    expected = {scope: sum(s[4] for case in pools.values() for s in case[scope]) for scope in ('realistic', 'added')}
    return {'spans_realistic': sum(len(c['realistic']) for c in pools.values()), 'spans_added_pool': sum(len(c['added']) for c in pools.values()),
            'expected_spans_realistic': expected['realistic'], 'expected_spans_added': expected['added'],
            'expected_spans_total': expected['realistic'] + expected['added'],
            'designated_units': sum(len(v) for v in designated.values()), 'cases_with_designated_units': sum(bool(v) for v in designated.values())}


def ranked(query_vector, vectors, items):
    """Rows in rank order with the expected flag; `items` aligned with `vectors`."""
    return [{'rank': r + 1, 'score': round(score, 6), **items[i]} for r, (i, score) in enumerate(rank(query_vector, vectors))]


def expected_ranks(rows):
    """Rank, pool size, score and margin to the best non-expected row; margin is None when either score is None
    (a pipeline row outside the reranked block carries no score)."""
    others = [o['score'] for o in rows if not o['expected'] and o['score'] is not None]
    best_other = max(others) if others else None
    return [{'rank': row['rank'], 'of': len(rows), 'score': row['score'], 'margin_to_best_other':
             None if row['score'] is None or best_other is None else round(row['score'] - best_other, 6)}
            for row in rows if row['expected']]


def mrr(ranks):
    return round(sum(1.0 / r['rank'] for r in ranks) / len(ranks), 4) if ranks else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', type=Path, required=True, help='record.json of the ranked-span run (build/ux4)')
    parser.add_argument('--product', type=Path, required=True, help='product-memory-use/<label> with units.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--batch', type=int, default=4, help='units per embedding request')
    parser.add_argument('--dry-run', action='store_true', help='print the pool sizes and expected-span counts without the embedder')
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    questions = {c['id']: c['question'] for c in fixture['cases']}
    record = json.loads(args.record.read_text())
    units = json.loads((args.product / 'units.json').read_text())
    pools = pools_from_record(record)
    designated = {c['id']: {uid for uid, flag in c['expected_answers'].items() if flag}
                  for c in json.loads((args.record.parent / 'plan.json').read_text())}
    if args.dry_run:
        print(json.dumps(pool_counts(pools, designated), indent=2)); return
    if sha(EMBED_GGUF) != EMBED_SHA:
        raise ValueError('Pinned embedding model mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (FIXTURE, args.record, args.record.parent / 'plan.json', args.product / 'units.json', Path(__file__)):
        shutil.copy2(source, inputs / source.name)
    binary = ROOT / 'build/llama/bin/llama-server'
    with private_socket() as sock:
        command = [str(binary), '-m', str(EMBED_GGUF), '-ngl', '99', '-fa', 'off', '--embedding', '--pooling', 'last',
                   '-np', '1', '-c', '2304', '-b', '2304', '-ub', '2304', '--cache-ram', '0', '--host', str(sock),
                   '--no-webui', '--no-slots']
        manifest = {'command': command, 'embed_model_sha256': EMBED_SHA, 'binary_sha256': sha(binary),
                    'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()}, 'instructions': INSTRUCTIONS,
                    'units': len(units), 'limits': 'Deterministic re-ranking of frozen pools and of all exported units under fixed '
                    'query instructions. Vector rank only; production retrieval also has lexical and planner stages. '
                    'Not a production change.'}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
        with session(args.output, command, sock):
            unit_vectors = []
            for i in range(0, len(units), args.batch):
                unit_vectors += embed(sock, [u['text'] for u in units[i:i + args.batch]], query=False)
            span_vectors = {}
            for cid, scopes in pools.items():
                for scope, pool in scopes.items():
                    if pool:
                        span_vectors[(cid, scope)] = embed(sock, [s[1] for s in pool], query=False)
            results = {}
            for name, instruction in INSTRUCTIONS.items():
                res = {'spans': {}, 'units': {}, 'units_documents_only': {}}
                for cid, question in questions.items():
                    qv = embed(sock, [question], query=True, instruction=instruction)[0]
                    for scope, pool in pools[cid].items():
                        if pool:
                            rows = ranked(qv, span_vectors[(cid, scope)],
                                          [{'label': s[0], 'unit_id': s[2], 'expected': s[4], 'span': s[1][:100]} for s in pool])
                            res['spans'][f'{cid}/{scope}'] = {'rows': rows, 'expected': expected_ranks(rows)}
                    if designated.get(cid):
                        items = [{'unit_id': u['unit_id'], 'kind': u['source_kind'], 'label': u['document_name'] or 'chat',
                                  'expected': u['unit_id'] in designated[cid]} for u in units]
                        rows = ranked(qv, unit_vectors, items)
                        res['units'][cid] = {'top5': [{k: r[k] for k in ('rank', 'score', 'label', 'expected')} for r in rows[:5]],
                                             'expected': expected_ranks(rows)}
                        doc_idx = [i for i, u in enumerate(units) if u['source_kind'] == 'document']
                        rows = ranked(qv, [unit_vectors[i] for i in doc_idx], [items[i] for i in doc_idx])
                        res['units_documents_only'][cid] = {'expected': expected_ranks(rows)}
                span_exp = [e for v in res['spans'].values() for e in v['expected']]
                unit_exp = [e for v in res['units'].values() for e in v['expected']]
                doc_exp = [e for v in res['units_documents_only'].values() for e in v['expected']]
                res['summary'] = {
                    'span_first_places': sum(e['rank'] == 1 for e in span_exp), 'span_expected': len(span_exp), 'span_mrr': mrr(span_exp),
                    'unit_first_places': sum(e['rank'] == 1 for e in unit_exp), 'unit_top5': sum(e['rank'] <= 5 for e in unit_exp),
                    'unit_expected': len(unit_exp), 'unit_mrr': mrr(unit_exp),
                    'documents_only_first_places': sum(e['rank'] == 1 for e in doc_exp), 'documents_only_top5': sum(e['rank'] <= 5 for e in doc_exp),
                    'documents_only_mrr': mrr(doc_exp)}
                results[name] = res
    # Determinism check against the ranked-span run: the production instruction must reproduce its ranks.
    previous = {f"{e['id']}/{'realistic' if scope == 'realistic' else 'added'}": [r['unit_id'] for r in rows]
                for e in record['cases'] for scope, rows in e['ranking'].items()}
    reproduced = {key: [r['unit_id'] for r in v['rows']] == previous.get(key) for key, v in results['production']['spans'].items() if key in previous}
    out = {'summary': {name: r['summary'] for name, r in results.items()}, 'production_reproduces_ux4_span_ranks': reproduced,
           'results': results}
    (args.output / 'ranking.json').write_text(json.dumps(out, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(out['summary'], indent=2)); print('reproduced ux4 ranks:', reproduced)


if __name__ == '__main__':
    main()
