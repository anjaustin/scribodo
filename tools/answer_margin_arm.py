#!/usr/bin/env python3
"""Margin arm: when the reranker's top is not decisive, let the embedder order the tie. Diagnostic only;
no production change.

Cycle 3 (journal/answer-cycle3_relation.md, arm 4) measured that the embedder sees whose attribute a passage
is about while the cross-encoder sees who is named, and that the two are each right where the other is wrong.
A margin rule ("defer to the embedder when the reranker's top is not decisive") fitted those seven cases and
was not proposed on seven. The second novel added the same failure in a new form: the Kantos Kan unit ranks
seventh under six units naming him, every one within 0.006 of the top (journal/answer-cycle4_mars.md).

This arm replays recorded live runs, so the pools and reranker scores are production's own (the
ANSWER_PIPELINE_SCORES lines, 12-hex unit prefixes resolved through the run's units.json). For every pooled
unit it adds the pinned production embedder's cosine against the question under the production query
instruction, and for every designated unit reports its reranker rank, its embedder rank within the pool and
among all of the run's document units, and the size of the reranker's tie at each margin. It then evaluates
deterministic slot rules over every case of every run given:

  today          the two highest reranker scores (answer_assign_slots, one part)
  embedder       the two highest embedder scores within the pool
  margin_M       the reranker's tie T = {u : score(u) >= top - M}; if |T| <= 2 today's slots, else the two
                 highest embedder scores within T
  fused_M        the same tie, ordered by reciprocal rank fusion of the two rankings within T (k = 60)
  window_M       the same tie, ordered by each unit's best three-sentence window under the reranker (cycle 3
                 arm 2's granularity, here confined to the tie so a decisive top is never disturbed)

scored as designated units in slots, beside the number of cases whose slots change at all (every change
on a case that was right is a loss to be seen, not averaged away). Reported and not ruled on: the designated
unit's place among the units that mention the question's longest capitalized phrase, in manuscript order
(the Kantos Kan unit is his first mention; a rule written on sight of one case is not proposed here).
One embedder session and one reranker session, pinned GGUFs and llama-server; unit scores are the run's,
only the windows of the widest tie are scored.
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
from answer_pipeline_review import designated_units, parse_trace, review  # noqa: E402
from answer_question_parts import clip_utf8  # noqa: E402
from answer_rerank_arm import rerank_prompt  # noqa: E402
from answer_rerank_runtime import GGUF, GGUF_SHA, llama_score, token_id  # noqa: E402
from answer_unit_extraction import EMBED_GGUF, EMBED_SHA, embed, session  # noqa: E402
from answer_window_rerank import windows  # noqa: E402
from memory_causal_parity import ROOT, sha  # noqa: E402

MARGINS = (0.002, 0.005, 0.01, 0.02, 0.03, 0.05)
RRF_K = 60
SLOTS = 2
WIDTH = 3  # sentences per window (cycle 3 arm 2, wr2)
CAPITALIZED = re.compile(r"\b[A-Z][a-z]+(?:['\u2019]s)?(?: [A-Z][a-z]+(?:['\u2019]s)?)*")


def tie(scored, margin):
    """The reranker's tie: unit ids within margin of the top score, in pool order."""
    if not scored:
        return []
    top = max(s for _, s in scored)
    return [u for u, s in scored if s >= top - margin]


def top_two(ids, score):
    """answer_assign_slots for one part: the highest, then the highest other; ties keep pool order."""
    chosen = []
    for _ in range(SLOTS):
        best = None
        for u in ids:
            if u in chosen:
                continue
            if best is None or score(u) > score(best):
                best = u
        if best is None:
            break
        chosen.append(best)
    return chosen


def slots_today(scored, embed_score, window_score=None):
    rerank = dict(scored)
    return top_two([u for u, _ in scored], lambda u: rerank[u])


def slots_embedder(scored, embed_score, window_score=None):
    return top_two([u for u, _ in scored], lambda u: embed_score[u])


def slots_margin(scored, embed_score, margin, window_score=None):
    tied = tie(scored, margin)
    if len(tied) <= SLOTS:
        return slots_today(scored, embed_score)
    return top_two(tied, lambda u: embed_score[u])


def slots_window(scored, embed_score, margin, window_score):
    """Within the tie, each unit by its best window; a unit without a window score (none scored) keeps its unit score."""
    tied = tie(scored, margin)
    if len(tied) <= SLOTS:
        return slots_today(scored, embed_score)
    rerank = dict(scored)
    return top_two(tied, lambda u: window_score[u] if window_score.get(u) is not None else rerank[u])


def rrf(ids, *rankings, k=RRF_K):
    """Reciprocal rank fusion within ids: each ranking is a score function, highest first; ties share the lower rank."""
    fused = {u: 0.0 for u in ids}
    for score in rankings:
        order = sorted(ids, key=lambda u: -score(u))
        for r, u in enumerate(order):
            fused[u] += 1.0 / (k + r + 1)
    return fused


def slots_fused(scored, embed_score, margin, window_score=None):
    tied = tie(scored, margin)
    if len(tied) <= SLOTS:
        return slots_today(scored, embed_score)
    rerank = dict(scored)
    fused = rrf(tied, lambda u: rerank[u], lambda u: embed_score[u])
    return top_two(tied, lambda u: fused[u])


def variants():
    """name -> rule(scored, embed_score, window_score) -> slots."""
    out = {'today': slots_today, 'embedder': slots_embedder}
    for m in MARGINS:
        out[f'margin_{m}'] = lambda s, e, w, m=m: slots_margin(s, e, m)
    for m in MARGINS:
        out[f'fused_{m}'] = lambda s, e, w, m=m: slots_fused(s, e, m)
    for m in MARGINS:
        out[f'window_{m}'] = lambda s, e, w, m=m: slots_window(s, e, m, w)
    return out


def name_phrase(question):
    """The question's longest run of capitalized words, possessives stripped; '' when it has none past the first word."""
    runs = [m.group(0) for m in CAPITALIZED.finditer(question) if m.start() > 0]
    if not runs:
        return ''
    best = max(runs, key=len)
    return re.sub(r"['\u2019]s\b", '', best)


def first_mention(phrase, docs, prefix):
    """(rank of the unit among the document units that contain the phrase, in manuscript order; their count)."""
    if not phrase:
        return None, 0
    ordered = sorted(docs, key=lambda u: (u['document_name'], int(u['byte_start'])))
    mentions = [u['unit_id'][:12] for u in ordered if phrase in u['text']]
    return (mentions.index(prefix) + 1 if prefix in mentions else None), len(mentions)


def cosine(a, b):
    return sum(x * y for x, y in zip(a, b))


def load_run(run, fixture_path):
    fixture = json.loads(fixture_path.read_text())
    messages = json.loads((run / 'messages.json').read_text())
    units = json.loads((run / 'units.json').read_text())
    traces = parse_trace((run / 'app.log').read_text(errors='replace'))
    sheet = review(fixture, messages, units, traces)
    documents = [u for u in units if u['source_kind'] == 'document']
    by_prefix = {u['unit_id'][:12]: u for u in documents}
    cases = []
    for c, case in zip(sheet, fixture['cases']):
        if 'error' in c or not case['designated']:
            continue
        scores = (c['trace'].get('part_scores') or {}).get(1, [])
        if not scores or c['trace'].get('parts') not in (None, 1):
            continue  # one pool for the whole question is the measured shape (pass 6); a parts run is not this arm's input
        pool = [(p, s) for p, s in scores if p in by_prefix]
        designated = [d['unit_id'][:12] for d in designated_units(case, units) if d['unit_id']]
        run_slots = [c['trace']['top'][:12]] + ([c['trace']['second'][:12]] if c['trace'].get('second') and c['trace']['second'] != '-' else [])
        cases.append({'id': case['id'], 'question': case['question'], 'pool': pool, 'dropped': len(scores) - len(pool),
                      'designated': designated, 'label': run.name, 'run_slots': run_slots})
    return fixture, documents, by_prefix, cases


def evaluate(cases, embed_scores, embed_rank_all, window_scores, mentions):
    """Rows per case and totals per variant. embed_scores[(case, run, prefix)] is the embedder cosine for the case's
    question; window_scores the best window score of the units in the widest tie; mentions[(case, run, prefix)] the
    first-mention diagnostic."""
    rules = variants()
    rows, totals = [], {name: {'designated_in_slots': 0, 'cases_changed': 0, 'cases_lost': 0, 'cases_gained': 0} for name in rules}
    for c in cases:
        escore = {u: embed_scores[(c['id'], c['label'], u)] for u, _ in c['pool']}
        wscore = {u: window_scores.get((c['id'], c['label'], u)) for u, _ in c['pool']}
        today = rules['today'](c['pool'], escore, wscore)
        rerank_rank = {u: r + 1 for r, (u, _) in enumerate(sorted(c['pool'], key=lambda x: -x[1]))}
        embed_rank = {u: r + 1 for r, u in enumerate(sorted(escore, key=lambda u: -escore[u]))}
        widest = tie(c['pool'], max(MARGINS))
        window_rank = {u: r + 1 for r, u in enumerate(sorted(widest, key=lambda u: -(wscore.get(u) or 0)))}
        row = {'id': c['id'], 'run': c['label'], 'pool': len(c['pool']), 'dropped_from_pool': c['dropped'],
               'designated': [{'unit': u, 'rerank_rank': rerank_rank.get(u), 'rerank_score': dict(c['pool']).get(u),
                               'embed_rank_in_pool': embed_rank.get(u), 'embed_score': escore.get(u),
                               'embed_rank_all_units': embed_rank_all.get((c['id'], c['label'], u)),
                               'window_score': wscore.get(u), 'window_rank_in_widest_tie': window_rank.get(u),
                               'first_mention': mentions.get((c['id'], c['label'], u))} for u in c['designated']],
               'tie_sizes': {str(m): len(tie(c['pool'], m)) for m in MARGINS}, 'windows_scored': sum(wscore.get(u) is not None for u in widest),
               'run_slots': c.get('run_slots'), 'today_matches_run': today == c.get('run_slots'), 'variants': {}}
        for name, rule in rules.items():
            slots = rule(c['pool'], escore, wscore)
            hits = sum(u in slots for u in c['designated'])
            today_hits = sum(u in today for u in c['designated'])
            row['variants'][name] = {'slots': slots, 'designated_in_slots': hits}
            t = totals[name]
            t['designated_in_slots'] += hits
            t['cases_changed'] += slots != today
            t['cases_lost'] += hits < today_hits
            t['cases_gained'] += hits > today_hits
        rows.append(row)
    return rows, totals


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--run', action='append', type=Path, required=True, help='a live run directory (app.log, messages.json, units.json); repeatable')
    parser.add_argument('--fixture', action='append', type=Path, required=True, help='the boundary fixture for the run in the same position; repeatable')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if len(args.run) != len(args.fixture):
        parser.error('one --fixture per --run')
    for path, expected in ((EMBED_GGUF, EMBED_SHA), (GGUF, GGUF_SHA)):
        if sha(path) != expected:
            raise ValueError(f'Pinned model mismatch: {path}')
    binary = ROOT / 'build/llama/bin/llama-server'
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    shutil.copy2(Path(__file__), inputs / Path(__file__).name)
    runs = []
    for run, fixture_path in zip(args.run, args.fixture):
        shutil.copy2(fixture_path, inputs / f'{run.name}-{fixture_path.name}')
        runs.append((run, fixture_path) + load_run(run, fixture_path))
    manifest = {'embedder_sha256': EMBED_SHA, 'reranker_sha256': GGUF_SHA, 'binary_sha256': sha(binary), 'margins': MARGINS, 'rrf_k': RRF_K, 'window_width': WIDTH,
                'runs': [{'run': str(run), 'fixture': str(fx), 'fixture_sha256': sha(fx), 'units_sha256': sha(run / 'units.json'),
                          'app_log_sha256': sha(run / 'app.log'), 'document_units': len(docs), 'cases': [c['id'] for c in cases]}
                         for run, fx, _, docs, _, cases in runs],
                'limits': 'Pools and reranker scores are the recorded run\'s (ANSWER_PIPELINE_SCORES, 12-hex prefixes, at most 64 scored). '
                          'The embedder score is computed here with the pinned GGUF on the unit text; production would have it for semantic '
                          'candidates at admission and would need one Qdrant lookup for lexical-only ones. Windows are scored for the units of '
                          'the widest tie only, with the production clip. One run per novel, pinned models.'}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    embed_scores, embed_rank_all, timings = {}, {}, {}
    with private_socket() as sock:
        command = [str(binary), '-m', str(EMBED_GGUF), '-ngl', '99', '-fa', 'off', '--embedding', '--pooling', 'last',
                   '-np', '1', '-c', '2304', '-b', '2304', '-ub', '2304', '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        embed_dir = args.output / 'embed'; embed_dir.mkdir()
        t0 = time.monotonic()
        with session(embed_dir, command, sock):
            for run, _, _, docs, by_prefix, cases in runs:
                vectors = []
                for i in range(0, len(docs), 4):
                    vectors += embed(sock, [u['text'] for u in docs[i:i + 4]], query=False)
                vector_of = {u['unit_id'][:12]: v for u, v in zip(docs, vectors)}
                for c in cases:
                    q = embed(sock, [c['question']], query=True)[0]
                    all_scores = sorted(((cosine(q, v), p) for p, v in vector_of.items()), reverse=True)
                    rank_all = {p: r + 1 for r, (_, p) in enumerate(all_scores)}
                    for p, _ in c['pool']:
                        embed_scores[(c['id'], run.name, p)] = cosine(q, vector_of[p])
                    for p in c['designated']:
                        embed_rank_all[(c['id'], run.name, p)] = rank_all.get(p)
        timings['embedder_seconds'] = round(time.monotonic() - t0, 1)
    # The first-mention diagnostic, model-free.
    mentions = {}
    for run, _, _, docs, by_prefix, cases in runs:
        for c in cases:
            phrase = name_phrase(c['question'])
            for p in c['designated']:
                rank_, count = first_mention(phrase, docs, p)
                mentions[(c['id'], run.name, p)] = {'phrase': phrase, 'rank': rank_, 'mentioning_units': count}
    # Stage 2: the reranker over every window of every unit in the widest tie.
    window_scores, pairs, failures = {}, 0, 0
    with private_socket() as sock:
        command = [str(binary), '-m', str(GGUF), '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16',
                   '-np', '1', '-c', '2048', '-b', '2048', '-ub', '2048', '--cache-ram', '0', '--host', str(sock), '--no-webui', '--no-slots']
        rerank_dir = args.output / 'rerank'; rerank_dir.mkdir()
        t0 = time.monotonic()
        with session(rerank_dir, command, sock):
            yes, no = token_id(sock, 'yes'), token_id(sock, 'no')
            for run, _, _, docs, by_prefix, cases in runs:
                for c in cases:
                    for p in tie(c['pool'], max(MARGINS)):
                        best = None
                        for _, text in windows(by_prefix[p]['text'], WIDTH):
                            pairs += 1
                            try:
                                score = llama_score(sock, rerank_prompt(c['question'], clip_utf8(text)), yes, no)[0]
                            except ValueError:
                                score = 0.0; failures += 1
                            best = score if best is None else max(best, score)
                        window_scores[(c['id'], run.name, p)] = best
        timings['reranker_seconds'] = round(time.monotonic() - t0, 1); timings['window_pairs'] = pairs; timings['score_failures'] = failures
    cases = [c for *_, cs in runs for c in cs]
    rows, totals = evaluate(cases, embed_scores, embed_rank_all, window_scores, mentions)
    designated_total = sum(len(c['designated']) for c in cases)
    results = {'timings': timings, 'designated_units': designated_total, 'cases': rows, 'variants': totals,
               'embed_scores': {f'{cid}|{label}|{p}': round(s, 4) for (cid, label, p), s in embed_scores.items()},
               'window_scores': {f'{cid}|{label}|{p}': (round(s, 4) if s is not None else None) for (cid, label, p), s in window_scores.items()}}
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    calibrated = sum(r['today_matches_run'] for r in rows)
    print(f"calibration: today's rule reproduces the run's slots on {calibrated} of {len(rows)} cases")
    print(f"{'variant':14s} designated_in_slots/{designated_total} cases_changed cases_gained cases_lost  (cases: {len(cases)})")
    for name, t in totals.items():
        print(f"{name:14s} {t['designated_in_slots']:19d} {t['cases_changed']:13d} {t['cases_gained']:12d} {t['cases_lost']:10d}")
    print()
    for row in rows:
        for d in row['designated']:
            fm = d['first_mention'] or {}
            print(f"{row['run']:12s} {row['id']:26s} rerank {str(d['rerank_rank']):>4s} ({d['rerank_score']}) embed-in-pool {str(d['embed_rank_in_pool']):>3s} "
                  f"embed-all {str(d['embed_rank_all_units']):>3s} window-in-tie {str(d['window_rank_in_widest_tie']):>3s} ({d['window_score']}) "
                  f"mention {fm.get('rank')}/{fm.get('mentioning_units')} {fm.get('phrase')!r} ties {row['tie_sizes']}")
    print(f"timings: {timings}")


if __name__ == '__main__':
    main()
