#!/usr/bin/env python3
"""Coreference-at-ingestion arm: does naming the pronoun subjects of the
manuscript's stating sentences, once per document at ingestion time, move the
designated units into the slots? Diagnostic only; no production change.

Cycle 3 (journal/answer-cycle3_relation.md) located the remaining selection
error of the verified pipeline: a question asks for one entity's attribute, the
manuscript states it with a pronoun subject six sentences after the name ("They
were rather handsome; ... had a fortune of twenty thousand pounds"), and other
passages name the entity without stating the attribute. The reranker's signal is
who is named, so the stating unit ranks under passages that name Mr. Bingley
and say "fortune". Every arm of cycle 3 asked the production reranker a question
it was not trained on; none changed what the unit says.

This arm changes the indexed text, not the original:
- a dedicated coreference model (fastcoref's FCoref or LingMess, a few hundred
  million parameters, run once per document on the CPU with the chat model
  unloaded) clusters the mentions of each document, chunked at unit boundaries
  with a context prefix so a pronoun early in a chunk can see its antecedent;
- deterministic software picks one representative per cluster (the most
  frequent short mention carrying a proper noun; else the earliest short
  nominal; else none), names the pronouns inside a nominal representative from
  their own clusters ("his sisters" to "Mr. Bingley’s sisters"), and rewrites
  only third-person pronoun mentions: replaced by the representative, or kept
  with the representative appended in brackets (--form);
- the document units of a live run are re-emitted with the resolved text (chat
  units untouched, ids and order unchanged) as a derived product directory, and
  tools/answer_admission_arm.py runs today's selection shape and its variants
  over it with the pinned embedder and reranker, exactly as it ran on the
  original units (build/ad2);
- the designated units' ranks and slots are compared, case by case, against
  that baseline.

The byte-verified quotation path is untouched by design: what the author would
see is still the original unit; only what is searched changes. Limits: one live
run's units, one fixture, two coreference models with published scores on news
text, not fiction; the lexical feeder is the arm's content-word stand-in.
"""
import argparse
from collections import Counter, namedtuple
from itertools import product
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from memory_causal_parity import ROOT, sha  # noqa: E402

FIXTURE = ROOT / 'tests/fixtures/answer_boundary/questions.json'
ADMISSION_ARM = ROOT / 'tools/answer_admission_arm.py'
MODELS = {'fcoref': 'biu-nlp/f-coref', 'lingmess': 'biu-nlp/lingmess-coref'}
FORMS = ('replace', 'annotate')
# Third-person pronouns are the only mentions rewritten. First and second person are speakers in dialogue and the
# models cluster them with whoever is talking; reflexives read badly when replaced; "it" and "its" are rewritten only
# to a named representative because their clusters are mostly events and things.
SUBJECT_OBJECT = {'he', 'she', 'they', 'him', 'them'}
NEUTER = {'it', 'its'}
POSSESSIVE_ONLY = {'his', 'their', 'hers', 'theirs', 'its'}  # "her" is possessive only when tagged PRP$
THIRD_PERSON = SUBJECT_OBJECT | NEUTER | POSSESSIVE_ONLY | {'her'}
MAX_REPRESENTATIVE_CHARS = 40
CORE_CHARS = 11000  # one chunk's own units, about 2,800 tokens
CONTEXT_CHARS = 2500  # the tail of the preceding units, seen but never rewritten
MAX_CHUNK_TOKENS = 3800  # LingMess skips any document over 4,096 encoder tokens; its word-split count runs a little over the raw one
APOSTROPHE = '’'  # the manuscript's own

Token = namedtuple('Token', 'start end text tag dep head_dep')
Mention = namedtuple('Mention', 'start end text cluster')
UNITS_OF_INTEREST = ('had a fortune of twenty thousand pounds', 'nearly a hundred thousand pounds',
                     'Miss de Bourgh, will have a very large fortune', 'entailed, in default of heirs male',
                     'about twenty-seven', 'only three miles', 'not sixteen')
SENTENCE_OPENERS = set('.!?“"‘[]\n')  # a token after one of these is sentence-initial: its capital proves nothing
SUBJECT_DEPS = ('nsubj', 'nsubjpass', 'csubj')
DENSITIES = ('all', 'subjects')


def chunks(spans, core_chars=CORE_CHARS, context_chars=CONTEXT_CHARS):
    """Consecutive unit spans grouped into chunks of at most `core_chars` (a unit longer than that is a chunk by
    itself), each with the start of its context prefix: `context_chars` back from the core start, but never before
    the document start. Yields (context_start, core_start, core_end)."""
    out = []
    i = 0
    while i < len(spans):
        start = spans[i][0]
        j = i
        while j + 1 < len(spans) and spans[j + 1][1] - start <= core_chars:
            j += 1
        end = spans[j][1]
        out.append((max(0, start - context_chars), start, end))
        i = j + 1
    return out


def tokens_in(tokens, start, end):
    return [t for t in tokens if t.start >= start and t.end <= end]


def proper_vocabulary(tokens):
    """The chunk's names: capitalized words the tagger calls proper nouns somewhere other than sentence-initially.
    "Vain indeed must be..." and "Affectation of candour..." are tagged NNP at a sentence start and are not names."""
    proper = set()
    for i, t in enumerate(tokens):
        if t.tag not in ('NNP', 'NNPS') or not t.text[:1].isupper():
            continue
        previous = tokens[i - 1] if i else None
        initial = previous is None or previous.text[-1] in SENTENCE_OPENERS or previous.text == '--' or '\n' in previous.text
        if not initial:
            proper.add(t.text)
    return proper


def is_named(mention, tokens, proper=None):
    proper = proper_vocabulary(tokens) if proper is None else proper
    return any(t.tag in ('NNP', 'NNPS') and t.text in proper for t in tokens_in(tokens, mention.start, mention.end))


def pronoun_kind(mention, tokens):
    """'possessive', 'plain', or None when the mention is not one third-person pronoun token."""
    inside = tokens_in(tokens, mention.start, mention.end)
    if len(inside) != 1 or mention.text.strip().lower() not in THIRD_PERSON:
        return None
    word = inside[0].text.lower()
    if word in POSSESSIVE_ONLY or inside[0].tag == 'PRP$':
        return 'possessive'
    return 'plain'


def is_subject(mention, tokens):
    """A clause subject, or the possessive of one: "They had a fortune", "His sisters were anxious"."""
    inside = tokens_in(tokens, mention.start, mention.end)
    if len(inside) != 1:
        return False
    t = inside[0]
    return t.dep in SUBJECT_DEPS or (t.dep == 'poss' and t.head_dep in SUBJECT_DEPS)


def contains_pronoun(mention, tokens):
    return any(t.text.lower() in THIRD_PERSON for t in tokens_in(tokens, mention.start, mention.end))


def strip_possessive(text):
    text = re.sub(r'\s+', ' ', text.strip())
    return re.sub(r"[’']s?$", '', text) if re.search(r"[’']s$|s[’']$", text) else text


def short_enough(mention):
    return len(strip_possessive(mention.text)) <= MAX_REPRESENTATIVE_CHARS and '\n' not in mention.text and ',' not in mention.text


def representative(mentions, tokens, proper=None):
    """The cluster's name: among short mentions with a proper noun and no pronoun or comma, the most frequent, then
    the shortest, then the earliest; else the earliest short mention that is not a pronoun; else None.
    Returns (text, mention, named)."""
    proper = proper_vocabulary(tokens) if proper is None else proper
    named = [m for m in mentions if is_named(m, tokens, proper) and short_enough(m) and not contains_pronoun(m, tokens)]
    if named:
        counts = Counter(strip_possessive(m.text) for m in named)
        best = max(named, key=lambda m: (counts[strip_possessive(m.text)], -len(strip_possessive(m.text)), -m.start))
        return strip_possessive(best.text), best, True
    nominal = [m for m in mentions if pronoun_kind(m, tokens) is None and m.text.strip().lower() not in THIRD_PERSON
               and short_enough(m)]
    if nominal:
        best = min(nominal, key=lambda m: m.start)
        return strip_possessive(best.text), best, False
    return None, None, False


def possessive(text):
    """"Mr. Bingley’s", "Mr. Collins’s", but a plural common noun takes the bare apostrophe: "the sisters’"."""
    last = text.split()[-1] if text.split() else text
    if last[:1].islower() and last.endswith('s'):
        return text + APOSTROPHE
    return text + APOSTROPHE + 's'


def nest(rep_text, rep_mention, named_reps, mentions, tokens, proper):
    """A nominal representative with the pronouns inside it named from their own clusters: "his sisters" with "his"
    in Mr. Bingley's cluster becomes "Mr. Bingley’s sisters". One level; only named inner representatives."""
    if rep_mention is None:
        return rep_text
    inner = [m for m in mentions if m.cluster != rep_mention.cluster and m.start >= rep_mention.start
             and m.end <= rep_mention.end and m.cluster in named_reps]
    edits = []
    for m in inner:
        kind = pronoun_kind(m, tokens)
        name = named_reps[m.cluster]
        if kind == 'possessive':
            edits.append((m.start, m.end, possessive(name)))
        elif kind == 'plain':
            edits.append((m.start, m.end, name))
        elif not is_named(m, tokens, proper) and re.search(r"[’']s$", m.text):
            edits.append((m.start, m.end, possessive(name)))  # "this man’s sisters"
    if not edits:
        return rep_text
    text = rep_mention.text
    for s, e, new in sorted(edits, reverse=True):
        text = text[:s - rep_mention.start] + new + text[e - rep_mention.start:]
    return strip_possessive(text) if re.search(r"[’']s$", rep_mention.text) else re.sub(r'\s+', ' ', text.strip())


def replacements(mentions, tokens, form, density='all'):
    """(start, end, new_text) for every third-person pronoun mention whose cluster has a representative, in chunk
    coordinates. `form` is 'replace' (the pronoun becomes the name) or 'annotate' (the name follows in brackets);
    `density` is 'all' or 'subjects' (clause subjects and their possessives only)."""
    proper = proper_vocabulary(tokens)
    by_cluster = {}
    for m in mentions:
        by_cluster.setdefault(m.cluster, []).append(m)
    reps, named_reps = {}, {}
    for c, ms in by_cluster.items():
        text, mention, named = representative(ms, tokens, proper)
        if text:
            reps[c] = (text, mention, named)
            if named:
                named_reps[c] = text
    out = []
    for c, (text, mention, named) in reps.items():
        name = text if named else nest(text, mention, named_reps, mentions, tokens, proper)
        for m in by_cluster[c]:
            kind = pronoun_kind(m, tokens)
            if kind is None or (m.text.lower() in NEUTER and not named):
                continue
            if density == 'subjects' and not is_subject(m, tokens):
                continue
            if name.lower() == m.text.lower():
                continue
            new = possessive(name) if kind == 'possessive' else name
            if m.text[0].isupper() and form == 'replace':
                new = new[0].upper() + new[1:]
            out.append((m.start, m.end, new if form == 'replace' else f'{m.text} [{new}]'))
    return sorted(out)


def apply(text, edits, offset=0):
    """`edits` are (start, end, new) in document coordinates; `text` starts at `offset`. Edits that cross the
    text's ends are skipped."""
    out = text
    for s, e, new in sorted(edits, reverse=True):
        if s < offset or e > offset + len(text):
            continue
        out = out[:s - offset] + new + out[e - offset:]
    return out


def load_models(name, device):
    """fastcoref and spaCy, imported here so the model-free tests and the comparison stage need neither."""
    import logging
    logging.disable(logging.INFO)
    from transformers import AutoModel
    original = AutoModel.from_config.__func__

    def eager(cls, config, **kw):  # the pinned transformers refuses Longformer without an explicit attention kind
        kw.setdefault('attn_implementation', 'eager')
        return original(cls, config, **kw)
    AutoModel.from_config = classmethod(eager)
    import spacy
    from fastcoref import FCoref, LingMessCoref
    nlp = spacy.load('en_core_web_sm', exclude=['ner', 'lemmatizer'])  # the parser marks clause subjects
    model = (FCoref if name == 'fcoref' else LingMessCoref)(device=device, enable_progress_bar=False)
    return model, nlp


def resolve_document(doc_text, spans, model, nlp, form, density, stats):
    """Every unit of one document rewritten from the clusters of its chunk. Returns the resolved texts in span order."""
    edits = []
    tokenizer = model.tokenizer
    for context_start, core_start, core_end in chunks(spans):
        while True:
            chunk = doc_text[context_start:core_end]
            n = len(tokenizer(chunk, add_special_tokens=True)['input_ids'])
            if n <= MAX_CHUNK_TOKENS or context_start >= core_start:
                break
            context_start = min(core_start, context_start + 500)
        if n > MAX_CHUNK_TOKENS:
            raise ValueError(f'A single unit span exceeds {MAX_CHUNK_TOKENS} tokens: {core_start}-{core_end}')
        stats['chunks'] += 1
        stats['max_chunk_tokens'] = max(stats['max_chunk_tokens'], n)
        t0 = time.monotonic()
        result = model.predict(texts=[chunk])
        stats['model_seconds'] += time.monotonic() - t0
        if len(result) != 1:
            raise RuntimeError('The coreference model returned no result for a chunk')
        tokens = [Token(t.idx, t.idx + len(t.text), t.text, t.tag_, t.dep_, t.head.dep_) for t in nlp(chunk)]
        mentions = [Mention(s, e, chunk[s:e], c) for c, cluster in enumerate(result[0].get_clusters(as_strings=False))
                    for s, e in cluster]
        stats['clusters'] += len(result[0].get_clusters(as_strings=False))
        stats['mentions'] += len(mentions)
        for s, e, new in replacements(mentions, tokens, form, density):
            if context_start + s >= core_start:  # the context prefix is seen, never rewritten
                edits.append((context_start + s, context_start + e, new))
    stats['pronouns_rewritten'] += len(edits)
    counts = [sum(1 for a, b, _ in edits if a >= s and b <= e) for s, e in spans]
    return [apply(doc_text[s:e], edits, s) for s, e in spans], counts


def resolve_product(product, output, model_name, form, density, device):
    """The derived product: units.json with document units resolved; a stats file; the units of interest."""
    units = json.load(open(product / 'units.json'))
    manuscript = product / 'work/work/manuscript'
    stats = {'chunks': 0, 'max_chunk_tokens': 0, 'clusters': 0, 'mentions': 0, 'pronouns_rewritten': 0,
             'model_seconds': 0.0, 'units_over_2000_bytes_before': 0, 'units_over_2000_bytes_after': 0}
    model, nlp = load_models(model_name, device)
    by_doc = {}
    for i, u in enumerate(units):
        if u['source_kind'] == 'document':
            by_doc.setdefault(u['document_name'], []).append(i)
    resolved = list(units)
    for name, indices in sorted(by_doc.items()):
        doc_text = (manuscript / name).read_text(encoding='utf-8')
        spans = []
        for i in indices:
            at = doc_text.find(units[i]['text'])
            if at < 0:
                raise ValueError(f'Unit {units[i]["unit_id"]} is not a substring of {name}')
            spans.append((at, at + len(units[i]['text'])))
        order = sorted(range(len(indices)), key=lambda k: spans[k][0])
        texts, counts = resolve_document(doc_text, [spans[k] for k in order], model, nlp, form, density, stats)
        for k, text, count in zip(order, texts, counts):
            i = indices[k]
            stats['units_over_2000_bytes_before'] += len(units[i]['text'].encode()) > 2000
            stats['units_over_2000_bytes_after'] += len(text.encode()) > 2000
            resolved[i] = {**units[i], 'text': text, 'resolved_from_text_hash': units[i]['text_hash'],
                           'rewrites': count}
    output.mkdir(parents=True, mode=0o700)
    (output / 'units.json').write_text(json.dumps(resolved, ensure_ascii=False, indent=1) + '\n')
    interest = []
    for u, r in zip(units, resolved):
        for passage in UNITS_OF_INTEREST:
            if u['source_kind'] == 'document' and passage in u['text']:
                at = r['text'].find(passage)
                interest.append({'passage': passage, 'document': u['document_name'], 'before': u['text'], 'after': r['text'],
                                 'rewrites': r.get('rewrites', 0), 'passage_survives': at >= 0,
                                 'passage_within_2000_byte_clip': at >= 0 and len(r['text'][:at].encode()) + len(passage.encode()) <= 2000})
    stats['model_seconds'] = round(stats['model_seconds'], 1)
    (output / 'stats.json').write_text(json.dumps(stats, indent=2) + '\n')
    (output / 'units_of_interest.json').write_text(json.dumps(interest, ensure_ascii=False, indent=1) + '\n')
    return stats, interest


def designated_rank(results, case_id, unit):
    """(reranker rank among the whole-question union32 pool, embedder rank in that pool, lexical score) or Nones."""
    scores = results['scores'][f'{case_id}|whole']
    u = str(unit)
    if u not in scores['rerank']:
        return None, None, None
    rerank = sorted(scores['rerank'], key=lambda k: (-scores['rerank'][k], int(k)))
    embed = sorted(scores['embed'], key=lambda k: (-scores['embed'][k], int(k)))
    return rerank.index(u) + 1, embed.index(u) + 1, scores['lexical'][u]


def compare(baseline, variants):
    """Per case and per designated unit: slot and ranks under today's shape, baseline against each variant; and the
    arm's variant totals side by side."""
    today = 'today_whole_union32_rerank_greedy'
    rows = []
    for row in baseline['cases']:
        cid = row['id']
        for v in variants.values():
            vrow = next(r for r in v['cases'] if r['id'] == cid)
            if vrow['designated'] != row['designated']:
                raise ValueError(f'{cid}: designated units differ after resolution: {row["designated"]} vs {vrow["designated"]}')
        for unit in row['designated']:
            entry = {'case': cid, 'unit': unit, 'baseline': {'in_slots': unit in row['variants'][today]['slots'],
                                                             'ranks': designated_rank(baseline, cid, unit)}}
            for name, v in variants.items():
                vrow = next(r for r in v['cases'] if r['id'] == cid)
                entry[name] = {'in_slots': unit in vrow['variants'][today]['slots'], 'ranks': designated_rank(v, cid, unit)}
            rows.append(entry)
    totals = {}
    for name in baseline['variants']:
        totals[name] = {'baseline': baseline['variants'][name], **{k: v['variants'][name] for k, v in variants.items()}}
    return {'designated': rows, 'totals': totals}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--product', type=Path, required=True, help='a live run directory: units.json and work/work/manuscript')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/ad2', help='an admission-arm run on the original units')
    parser.add_argument('--models', default='fcoref,lingmess')
    parser.add_argument('--forms', default='replace')
    parser.add_argument('--densities', default='all,subjects')
    parser.add_argument('--device', default='cpu')
    parser.add_argument('--admission-python', default='python3', help='interpreter for the admission arm (stdlib only)')
    parser.add_argument('--skip-admission', action='store_true', help='resolve and report the units of interest only')
    args = parser.parse_args()
    if sha(args.baseline / 'inputs/answer_admission_arm.py') != sha(ADMISSION_ARM):
        raise ValueError('The baseline was not produced by the current admission arm')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    inputs = args.output / 'inputs'; inputs.mkdir()
    for source in (FIXTURE, Path(__file__), ADMISSION_ARM):
        shutil.copy2(source, inputs / source.name)
    manifest = {'product': str(args.product), 'units_sha256': sha(args.product / 'units.json'), 'baseline': str(args.baseline),
                'baseline_results_sha256': sha(args.baseline / 'results.json'), 'models': {m: MODELS[m] for m in args.models.split(',')},
                'forms': args.forms.split(','), 'densities': args.densities.split(','), 'device': args.device, 'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()},
                'chunking': {'core_chars': CORE_CHARS, 'context_chars': CONTEXT_CHARS, 'max_chunk_tokens': MAX_CHUNK_TOKENS},
                'limits': 'One live run, one fixture. Coreference models published on news text; fiction unmeasured by their '
                          'authors. The admission arm\'s lexical feeder is a content-word stand-in. Selection only: no generation.'}
    try:
        import fastcoref, spacy, torch, transformers  # noqa: F401
        from importlib.metadata import version
        manifest['versions'] = {'fastcoref': version('fastcoref'), 'spacy': spacy.__version__, 'torch': torch.__version__,
                                'transformers': transformers.__version__}
    except ImportError:
        pass
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    variants, stats_all = {}, {}
    for model_name in args.models.split(','):
        for form, density in product(args.forms.split(','), args.densities.split(',')):
            name = f'{model_name}_{form}_{density}'
            derived = args.output / f'product-{name}'
            t0 = time.monotonic()
            stats, interest = resolve_product(args.product, derived, model_name, form, density, args.device)
            stats['wall_seconds'] = round(time.monotonic() - t0, 1)
            stats_all[name] = stats
            print(name, json.dumps(stats))
            for row in interest:
                print(f"  [{row['passage'][:30]}] survives={row['passage_survives']} in_clip={row['passage_within_2000_byte_clip']}")
            if args.skip_admission:
                continue
            admission = args.output / f'admission-{name}'
            subprocess.run([args.admission_python, str(ADMISSION_ARM), '--product', str(derived), '--output', str(admission)],
                           check=True, stdout=open(args.output / f'admission-{name}.log', 'w'), stderr=subprocess.STDOUT)
            variants[name] = json.loads((admission / 'results.json').read_text())
    (args.output / 'stats.json').write_text(json.dumps(stats_all, indent=2) + '\n')
    if args.skip_admission:
        return
    baseline = json.loads((args.baseline / 'results.json').read_text())
    report = compare(baseline, variants)
    (args.output / 'compare.json').write_text(json.dumps(report, indent=2) + '\n')
    names = list(variants)
    print(f"\n{'case':28s} {'unit':>5s}  {'baseline':>22s}  " + '  '.join(f'{n:>22s}' for n in names))
    print(f"{'':28s} {'':>5s}  {'slot rr/emb/lex':>22s}  " + '  '.join(f"{'slot rr/emb/lex':>22s}" for _ in names))
    for row in report['designated']:
        def cell(d):
            r = d['ranks']
            return f"{'SLOT' if d['in_slots'] else '-':>5s} {str(r[0]):>3s}/{str(r[1]):>3s}/{'-' if r[2] is None else f'{r[2]:.2f}':>5s}"
        print(f"{row['case']:28s} {row['unit']:5d}  {cell(row['baseline']):>22s}  " + '  '.join(f'{cell(row[n]):>22s}' for n in names))
    print(f"\n{'variant':40s} " + ' '.join(f'{n:>22s}' for n in ['baseline'] + names) + '   (slots_designated / pool_designated / cases_complete)')
    for name, t in report['totals'].items():
        print(f'{name:40s} ' + ' '.join(f"{t[k]['slots_designated']:>8d}/{t[k]['pool_designated']:>3d}/{t[k]['cases_all_designated']:>3d}     "
                                        for k in ['baseline'] + names))


if __name__ == '__main__':
    main()
