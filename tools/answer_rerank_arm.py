#!/usr/bin/env python3
"""Cross-encoder and lexical-overlap rankers against the production embedder; diagnostic only.

Four rankers place each designated unit among all exported units of the
boundary run, and each stating span within its verified span pool (build/ux4):

  embedder      Qwen3-Embedding bi-encoder, production instruction (the ri1 baseline)
  lexical       content-word overlap between question and text, deterministic
  cross         Qwen3-Reranker-0.6B reading question and text together, over every unit
  cross_top32   the same cross-encoder over the embedder's top 32 only (the rerank pipeline shape)

The reranker runs in PyTorch from the diagnostic venv (build/venv-causal), pinned
by Hugging Face revision and file hash, with the model card's scoring: the
probability of "yes" against "no" at the final position under the card's prompt
and its default instruction. No production component changes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answer_bedrock_probe import private_socket  # noqa: E402
from answer_ranker_instruction import expected_ranks, mrr, pools_from_record, ranked  # noqa: E402
from answer_unit_extraction import EMBED_GGUF, EMBED_SHA, QUERY_INSTRUCTION, embed, session  # noqa: E402
from memory_causal_parity import ROOT, sha  # noqa: E402

FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'
RERANKER = 'Qwen/Qwen3-Reranker-0.6B'
RERANKER_REVISION = 'e61197ed45024b0ed8a2d74b80b4d909f1255473'
# The model card's prompt, byte for byte, and its default instruction.
RERANK_PREFIX = ('<|im_start|>system\nJudge whether the Document meets the requirements based on the Query and the Instruct '
                 'provided. Note that the answer can only be "yes" or "no".<|im_end|>\n<|im_start|>user\n')
RERANK_SUFFIX = '<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
RERANK_INSTRUCTION = 'Given a web search query, retrieve relevant passages that answer the query'
TOP_N = 32
STOPWORDS = set('''a an the and or but of in on at to for from by with about as into like through after over between out
against during without before under around among is are was were be been being am do does did doing have has had having
i me my mine you your yours he him his she her hers it its they them their theirs we us our ours this that these those
who whom whose which what when where why how there here then than too very can could will would shall should may might
must not no nor so if because while until up down off again further once all any both each few more most other some such
only own same s t just don now did'''.split())


def content_words(text):
    return [w for w in re.findall(r"[a-z]+(?:'[a-z]+)?", text.lower()) if w not in STOPWORDS and len(w) > 1]


def lexical_score(question, text):
    """Share of the question's distinct content words present in the text (whole words, case-insensitive)."""
    q = set(content_words(question))
    if not q:
        return 0.0
    words = set(content_words(text))
    return len(q & words) / len(q)


def rerank_prompt(question, document, instruction=RERANK_INSTRUCTION):
    return f'{RERANK_PREFIX}<Instruct>: {instruction}\n<Query>: {question}\n<Document>: {document}{RERANK_SUFFIX}'


def order(scores):
    """(index, score) highest first, ties by index: the same rule as the embedder ranker."""
    return sorted(enumerate(scores), key=lambda s: (-s[1], s[0]))


def rows_from_scores(scores, items):
    return [{'rank': r + 1, 'score': round(float(score), 6), **items[i]} for r, (i, score) in enumerate(order(scores))]


def documents_only(rows):
    """The same order restricted to document units (chat units, which include the session's own turns, removed)."""
    docs = [r for r in rows if r['kind'] == 'document']
    rerank = [{**r, 'rank': i + 1} for i, r in enumerate(docs)]
    return {'expected': expected_ranks(rerank), 'top3': [{k: r[k] for k in ('rank', 'score', 'label', 'unit_id', 'expected')} for r in rerank[:3]]}


def pipeline_rows(embedder_rows, cross_scores_by_unit, items_by_unit, top_n=TOP_N):
    """Cross-encoder over the embedder's top N; everything outside keeps its embedder rank below the reranked block."""
    head = embedder_rows[:top_n]
    reranked = sorted(head, key=lambda r: (-cross_scores_by_unit[r['unit_id']], r['rank']))
    rows = [{**items_by_unit[r['unit_id']], 'rank': i + 1, 'score': round(float(cross_scores_by_unit[r['unit_id']]), 6)}
            for i, r in enumerate(reranked)]
    rows += [{**items_by_unit[r['unit_id']], 'rank': top_n + j + 1, 'score': None} for j, r in enumerate(embedder_rows[top_n:])]
    return rows


class Reranker:
    """Qwen3-Reranker scoring per the model card: P(yes) over {yes, no} logits at the last token."""

    def __init__(self, snapshot, device):
        import torch
        from transformers import AutoModelForCausalLM, AutoTokenizer
        self.torch = torch
        self.tokenizer = AutoTokenizer.from_pretrained(snapshot, padding_side='left')
        self.model = AutoModelForCausalLM.from_pretrained(snapshot, dtype=torch.float32).to(device).eval()
        self.device = device
        self.yes = self.tokenizer.convert_tokens_to_ids('yes')
        self.no = self.tokenizer.convert_tokens_to_ids('no')

    def scores(self, prompts, batch=4, max_length=1536):
        """Only the final position's logits are materialized (logits_to_keep=1): the full batch x sequence x vocabulary
        tensor in float32 exhausted Metal memory on the first run (build/rk1-failed)."""
        out = []
        torch = self.torch
        for i in range(0, len(prompts), batch):
            enc = self.tokenizer(prompts[i:i + batch], padding=True, truncation=True, max_length=max_length, return_tensors='pt').to(self.device)
            with torch.no_grad():
                logits = self.model(**enc, logits_to_keep=1).logits[:, -1, :]
            pair = torch.stack([logits[:, self.no], logits[:, self.yes]], dim=1)
            out += torch.nn.functional.log_softmax(pair, dim=1)[:, 1].exp().tolist()
        return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', type=Path, required=True, help='record.json of the ranked-span run (build/ux4)')
    parser.add_argument('--product', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device', default='mps')
    parser.add_argument('--dry-run', action='store_true', help='print pool and unit counts; no models')
    args = parser.parse_args()
    fixture = json.loads(FIXTURE.read_text())
    questions = {c['id']: c['question'] for c in fixture['cases']}
    record = json.loads(args.record.read_text())
    units = json.loads((args.product / 'units.json').read_text())
    pools = pools_from_record(record)
    designated = {c['id']: {uid for uid, flag in c['expected_answers'].items() if flag}
                  for c in json.loads((args.record.parent / 'plan.json').read_text())}
    if args.dry_run:
        print(json.dumps({'units': len(units), 'questions_with_designated': sum(bool(v) for v in designated.values()),
                          'designated_units': sum(len(v) for v in designated.values()),
                          'span_pools': {k: {s: len(p) for s, p in v.items() if p} for k, v in pools.items()},
                          'cross_pairs_units': len(units) * sum(bool(v) for v in designated.values())}, indent=2)); return
    from huggingface_hub import snapshot_download
    snapshot = Path(snapshot_download(RERANKER, revision=RERANKER_REVISION))
    weights = snapshot / 'model.safetensors'
    if sha(EMBED_GGUF) != EMBED_SHA:
        raise ValueError('Pinned embedding model mismatch')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (FIXTURE, args.record, args.record.parent / 'plan.json', args.product / 'units.json', Path(__file__)):
        shutil.copy2(source, inputs / source.name)
    import torch
    import transformers
    manifest = {'reranker': RERANKER, 'reranker_revision': RERANKER_REVISION, 'reranker_weights_sha256': sha(weights),
                'reranker_prompt_sha256': hashlib.sha256((RERANK_PREFIX + RERANK_SUFFIX + RERANK_INSTRUCTION).encode()).hexdigest(),
                'reranker_instruction': RERANK_INSTRUCTION, 'device': args.device, 'dtype': 'float32',
                'torch': torch.__version__, 'transformers': transformers.__version__,
                'embed_model_sha256': EMBED_SHA, 'embed_query_instruction': QUERY_INSTRUCTION, 'top_n': TOP_N,
                'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()}, 'units': len(units),
                'limits': 'Diagnostic ranking comparison. Vector rank is only the embedder component of production retrieval. '
                          'Cross-encoder uses the model card default instruction, not a tuned one. Not a production change.'}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    binary = ROOT / 'build/llama/bin/llama-server'
    results = {'units': {}, 'spans': {}}
    unit_rows = {}
    timings = {}
    # Embedder baseline (same shape as ri1).
    with private_socket() as sock:
        command = [str(binary), '-m', str(EMBED_GGUF), '-ngl', '99', '-fa', 'off', '--embedding', '--pooling', 'last',
                   '-np', '1', '-c', '2304', '-b', '2304', '-ub', '2304', '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        embed_dir = args.output / 'embed'; embed_dir.mkdir()
        t0 = time.monotonic()
        with session(embed_dir, command, sock):
            unit_vectors = []
            for i in range(0, len(units), 4):
                unit_vectors += embed(sock, [u['text'] for u in units[i:i + 4]], query=False)
            query_vectors = {cid: embed(sock, [q], query=True)[0] for cid, q in questions.items()}
            span_vectors = {(cid, scope): embed(sock, [s[1] for s in pool], query=False)
                            for cid, scopes in pools.items() for scope, pool in scopes.items() if pool}
        timings['embedder_seconds'] = round(time.monotonic() - t0, 1)
    items = [{'unit_id': u['unit_id'], 'kind': u['source_kind'], 'label': u['document_name'] or 'chat'} for u in units]
    t0 = time.monotonic()
    reranker = Reranker(snapshot, args.device)
    timings['reranker_load_seconds'] = round(time.monotonic() - t0, 1)
    t0 = time.monotonic()
    for cid, question in questions.items():
        if not designated.get(cid):
            continue
        flagged = [{**it, 'expected': it['unit_id'] in designated[cid]} for it in items]
        by_unit = {it['unit_id']: it for it in flagged}
        emb_rows = ranked(query_vectors[cid], unit_vectors, flagged)
        lex_rows = rows_from_scores([lexical_score(question, u['text']) for u in units], flagged)
        cross = reranker.scores([rerank_prompt(question, u['text']) for u in units])
        cross_rows = rows_from_scores(cross, flagged)
        cross_by_unit = {u['unit_id']: cross[i] for i, u in enumerate(units)}
        pipe_rows = pipeline_rows(emb_rows, cross_by_unit, by_unit)
        rankers = (('embedder', emb_rows), ('lexical', lex_rows), ('cross', cross_rows), ('cross_top32', pipe_rows))
        results['units'][cid] = {name: {'expected': expected_ranks(rows),
                                        'top5': [{k: r[k] for k in ('rank', 'score', 'label', 'unit_id', 'kind', 'expected')} for r in rows[:5]],
                                        'documents_only': documents_only(rows)}
                                 for name, rows in rankers}
        unit_rows[cid] = {name: [{k: r[k] for k in ('rank', 'score', 'unit_id', 'kind', 'expected')} for r in rows] for name, rows in rankers}
        results['units'][cid]['designated_cross_scores'] = {uid[:12]: round(cross_by_unit[uid], 6) for uid in designated[cid]}
        results['units'][cid]['lexical_question_words'] = content_words(question)
    for cid, scopes in pools.items():
        for scope, pool in scopes.items():
            if not pool:
                continue
            flagged = [{'label': s[0], 'unit_id': s[2], 'expected': s[4], 'span': s[1][:100]} for s in pool]
            emb_rows = ranked(query_vectors[cid], span_vectors[(cid, scope)], flagged)
            lex_rows = rows_from_scores([lexical_score(questions[cid], s[1]) for s in pool], flagged)
            cross_rows = rows_from_scores(reranker.scores([rerank_prompt(questions[cid], s[1]) for s in pool]), flagged)
            results['spans'][f'{cid}/{scope}'] = {name: {'expected': expected_ranks(rows), 'rows': rows}
                                                  for name, rows in (('embedder', emb_rows), ('lexical', lex_rows), ('cross', cross_rows))}
    timings['reranker_scoring_seconds'] = round(time.monotonic() - t0, 1)
    summary = {}
    for name in ('embedder', 'lexical', 'cross', 'cross_top32'):
        unit_exp = [e for v in results['units'].values() for e in v[name]['expected']]
        doc_exp = [e for v in results['units'].values() for e in v[name]['documents_only']['expected']]
        span_exp = [e for v in results['spans'].values() for e in v[name]['expected']] if name != 'cross_top32' else []
        summary[name] = {'unit_first_places': sum(e['rank'] == 1 for e in unit_exp), 'unit_top5': sum(e['rank'] <= 5 for e in unit_exp),
                         'unit_top16': sum(e['rank'] <= 16 for e in unit_exp), 'unit_expected': len(unit_exp), 'unit_mrr': mrr(unit_exp),
                         'documents_only_first_places': sum(e['rank'] == 1 for e in doc_exp), 'documents_only_top5': sum(e['rank'] <= 5 for e in doc_exp),
                         'documents_only_top16': sum(e['rank'] <= 16 for e in doc_exp), 'documents_only_mrr': mrr(doc_exp),
                         'span_first_places': sum(e['rank'] == 1 for e in span_exp), 'span_expected': len(span_exp), 'span_mrr': mrr(span_exp)}
    out = {'summary': summary, 'timings': timings, 'results': results}
    (args.output / 'ranking.json').write_text(json.dumps(out, ensure_ascii=False, indent=2) + '\n')
    (args.output / 'unit-rows.json').write_text(json.dumps(unit_rows, ensure_ascii=False) + '\n')
    print(json.dumps({'summary': summary, 'timings': timings}, indent=2))


if __name__ == '__main__':
    main()
