"""Span-verifier tolerance: what the normalization behind `answer_verify_span` accepts.

Every verified span a recorded run showed is taken with its unit's text from the same run's unit export. Each pair
is then mutated the ways a model's rendering could differ from the manuscript, in three families:

  orthographic   case, quotation marks, dashes, whitespace, emphasis markers: differences in rendering, not in words;
                 the verifier is meant to accept these and show the manuscript's bytes;
  meaning        a negation added or removed, a name replaced, a word dropped, two words swapped, an apostrophe lost
                 inside a word (it's -> its), a hyphen joined (well-known -> wellknown), a number changed: differences
                 in words; the verifier must reject these;
  edge           the cases the rules make hard to tell from rendering: a plural possessive apostrophe lost (the
                 Bennets' house -> the Bennets house), a marker inside a word (Eliza_beth), a hyphen split into a
                 space (well-known -> well known), a fabricated sentence appended, the last sentence of a
                 multi-sentence span altered. What the verifier does with these is the tolerance this tool measures.

A mutation is applied only where it applies (a span without a hyphen has no hyphen case); every meaning mutation
is made inside the first sentence so the prefix rule cannot rescue it. The binary is the product's verifier
(`build/answer_gate_replay`, {"span","unit"} items). No model.

usage: python3 tools/answer_span_tolerance.py [--runs DIR] [--binary PATH] [--examples N]
"""
import argparse
import json
import re
import subprocess
import sys
from collections import Counter, OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_HEADER = re.compile(r'--- VERIFIED SOURCE(?: 2)? \| unit=([0-9a-f]{64}) \| document=\S+ \| score=\S+ \| status=(\S+)')
SENTENCE_END = re.compile(r'[.!?]["\'”’]?(?=\s|$)')
AUXILIARIES = (' is ', ' was ', ' had ', ' has ', ' are ', ' were ', ' did ', ' will ', ' would ', ' could ', ' should ', ' can ',
               ' must ', ' might ')
NOT_NAMES = {'The', 'This', 'That', 'These', 'Those', 'It', 'So', 'If', 'Let', 'However', 'According', 'From', 'Based', 'She',
             'He', 'They', 'We', 'You', 'I', 'Who', 'Whose', 'What', 'Which', 'When', 'Where', 'Why', 'How', 'Yes', 'No', 'Here',
             'There', 'Then', 'Thus', 'Now', 'Mr', 'Mrs', 'Miss', 'Sir', 'Lady', 'Dear', 'Oh', 'But', 'And', 'For', 'In', 'On', 'At',
             'To', 'Of', 'By', 'With', 'As', 'My', 'Your', 'His', 'Her', 'Their', 'Our', 'Its', 'Not', 'Nor', 'Or', 'Yet', 'Well'}
NUMBER_WORDS = ['one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine', 'ten', 'twenty', 'thirty', 'forty', 'fifty',
                'hundred', 'thousand']


def load_pairs(runs_dir):
    """(run, recorded status, shown span, unit text) for every verified source block in every recorded run."""
    pairs = []
    for d in sorted(p for p in Path(runs_dir).iterdir() if (p / 'units.json').exists() and (p / 'messages.json').exists()):
        units = json.loads((d / 'units.json').read_text())
        units = units if isinstance(units, list) else units.get('units', units)
        by_id = {u['unit_id']: u.get('text') or '' for u in units}
        messages = json.loads((d / 'messages.json').read_text())
        messages = messages if isinstance(messages, list) else messages.get('messages', messages)
        for m in messages:
            sources = (m.get('memory_sources') or '') if isinstance(m, dict) else ''
            for part in re.split(r'\n(?=--- )', sources):
                header = SOURCE_HEADER.match(part)
                if not header:
                    continue
                lines = part.split('\n', 1)
                span = lines[1].rstrip('\n') if len(lines) > 1 else ''
                unit = by_id.get(header.group(1))
                if span.strip() and unit is not None:
                    pairs.append((d.name, header.group(2), span, unit))
    return pairs


def first_sentence_end(span):
    """Index just past the first sentence's end mark, or len(span) when the span is one sentence."""
    m = SENTENCE_END.search(span)
    return m.end() if m and m.end() < len(span) else len(span)


def words_of(text):
    return [(m.start(), m.end()) for m in re.finditer(r"[A-Za-z][A-Za-z'’]*", text)]


def splice(span, start, end, replacement):
    return span[:start] + replacement + span[end:]


# ---- orthographic -----------------------------------------------------------

def m_upper(span, unit):
    return span.upper() if span.upper() != span else None


def m_straight_quotes(span, unit):
    out = span.replace('“', '"').replace('”', '"').replace('‘', "'").replace('’', "'")
    return out if out != span else None


def m_no_double_quotes(span, unit):
    out = re.sub(r'["“”]', '', span)
    return out if out != span else None


def m_spaced_hyphen_for_dash(span, unit):
    out = re.sub(r'\s*[—–]\s*', ' - ', span)
    return out if out != span else None


def m_doubled_spaces(span, unit):
    return span.replace(' ', '  ') if ' ' in span else None


def m_drop_emphasis(span, unit):
    out = re.sub(r'[*_]', '', span)
    return out if out != span else None


# ---- meaning (inside the first sentence) ------------------------------------

def m_negate(span, unit):
    end = first_sentence_end(span)
    head = span[:end]
    m = re.search(r"\bnot\b ?|n't\b", head)
    if m:
        return splice(span, m.start(), m.end(), '' if m.group().startswith('not') else '')
    for aux in AUXILIARIES:
        i = head.find(aux)
        if i >= 0:
            return splice(span, i + len(aux), i + len(aux), 'not ')
    return None


def m_replace_name(span, unit):
    end = first_sentence_end(span)
    cands = [(s, e) for s, e in words_of(span[:end]) if s > 0 and span[s].isupper() and span[s:e].rstrip("'’s") not in NOT_NAMES
             and not span[s:e].isupper() and len(span[s:e]) >= 3 and not re.search(r'[.!?]\s*$', span[:s].rstrip('"“‘\''))]
    if not cands:
        return None
    s, e = cands[0]
    word = span[s:e]
    others = sorted({w for w in re.findall(r'\b[A-Z][a-z]{2,}\b', unit) if w not in NOT_NAMES and w != word.rstrip("'’s")})
    other = next((w for w in others if w.lower() not in span.lower()), None) or 'Nobody'
    tail = word[len(word.rstrip("'’s")):]
    return splice(span, s, e, other + tail)


def m_drop_word(span, unit):
    end = first_sentence_end(span)
    ws = [(s, e) for s, e in words_of(span[:end]) if e - s >= 3]
    if len(ws) < 4:
        return None
    s, e = ws[len(ws) // 2]
    while e < len(span) and span[e] == ' ':
        e += 1
    return splice(span, s, e, '')


def m_swap_words(span, unit):
    end = first_sentence_end(span)
    ws = [(s, e) for s, e in words_of(span[:end])]
    if len(ws) < 4:
        return None
    i = len(ws) // 2
    (s1, e1), (s2, e2) = ws[i], ws[i + 1]
    if span[s1:e1].lower() == span[s2:e2].lower():
        return None
    return span[:s1] + span[s2:e2] + span[e1:s2] + span[s1:e1] + span[e2:]


def m_lose_inner_apostrophe(span, unit):
    end = first_sentence_end(span)
    m = re.search(r"[A-Za-z]['’][A-Za-z]", span[:end])
    return splice(span, m.start() + 1, m.start() + 2, '') if m else None


def m_join_hyphen(span, unit):
    end = first_sentence_end(span)
    m = re.search(r'[A-Za-z]-[A-Za-z]', span[:end])
    return splice(span, m.start() + 1, m.start() + 2, '') if m else None


def m_change_number(span, unit):
    end = first_sentence_end(span)
    m = re.search(r'\d', span[:end])
    if m:
        return splice(span, m.start(), m.end(), str((int(m.group()) + 1) % 10))
    for w in NUMBER_WORDS:
        m = re.search(r'\b' + w + r'\b', span[:end], re.IGNORECASE)
        if m:
            other = NUMBER_WORDS[(NUMBER_WORDS.index(w) + 1) % len(NUMBER_WORDS)]
            return splice(span, m.start(), m.end(), other)
    return None


# ---- edge -------------------------------------------------------------------

def m_lose_plural_possessive(span, unit):
    m = re.search(r"s['’](?=\s)", span)
    return splice(span, m.start() + 1, m.start() + 2, '') if m else None


def m_marker_inside_word(span, unit):
    end = first_sentence_end(span)
    ws = [(s, e) for s, e in words_of(span[:end]) if e - s >= 6 and span[s:e].isalpha()]
    if not ws:
        return None
    s, e = ws[0]
    return splice(span, s + 3, s + 3, '_')


def m_split_hyphen(span, unit):
    end = first_sentence_end(span)
    m = re.search(r'[A-Za-z]-[A-Za-z]', span[:end])
    return splice(span, m.start() + 1, m.start() + 2, ' ') if m else None


def m_append_sentence(span, unit):
    return span + ' Nobody in the house knew of it.' if re.search(r'[.!?]["\'”’]?$', span) else None


def m_negate_last_sentence(span, unit):
    end = first_sentence_end(span)
    if end >= len(span):
        return None
    tail = span[end:]
    for aux in AUXILIARIES:
        i = tail.find(aux)
        if i >= 0:
            return span[:end] + splice(tail, i + len(aux), i + len(aux), 'not ')
    return None


FAMILIES = OrderedDict([
    ('orthographic', [('upper case', m_upper), ('straight quotes', m_straight_quotes), ('double quotes removed', m_no_double_quotes),
                      ('spaced hyphen for a dash', m_spaced_hyphen_for_dash), ('doubled spaces', m_doubled_spaces),
                      ('emphasis markers removed', m_drop_emphasis)]),
    ('meaning', [('negation added or removed', m_negate), ('name replaced', m_replace_name), ('word dropped', m_drop_word),
                 ('adjacent words swapped', m_swap_words), ('apostrophe lost inside a word', m_lose_inner_apostrophe),
                 ('hyphen joined', m_join_hyphen), ('number changed', m_change_number)]),
    ('edge', [('plural possessive apostrophe lost', m_lose_plural_possessive), ('marker inside a word', m_marker_inside_word),
              ('hyphen split', m_split_hyphen), ('fabricated sentence appended', m_append_sentence),
              ('last sentence negated', m_negate_last_sentence)]),
])


def verify(binary, items):
    out = subprocess.run([str(binary)], input=json.dumps(items), capture_output=True, text=True, check=True)
    return [json.loads(line) for line in out.stdout.splitlines()]


def measure(pairs, binary):
    """Per mutation: how many pairs it applied to and what the verifier returned for each; plus the unchanged spans."""
    items, keys = [], []
    for run, status, span, unit in pairs:
        items.append({'span': span, 'unit': unit}); keys.append(('unchanged', 'unchanged', run, span, span))
        for family, mutations in FAMILIES.items():
            for name, fn in mutations:
                mutated = fn(span, unit)
                if mutated is not None and mutated != span:
                    items.append({'span': mutated, 'unit': unit}); keys.append((family, name, run, span, mutated))
    results = verify(binary, items)
    table = OrderedDict()
    examples = {}
    for (family, name, run, span, mutated), r in zip(keys, results):
        row = table.setdefault((family, name), Counter())
        row[r['status']] += 1
        examples.setdefault((family, name, r['status']), (run, span, mutated, r.get('verified')))
    return table, examples


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--runs', default=str(ROOT / 'build/product-memory-use'))
    ap.add_argument('--binary', default=str(ROOT / 'build/answer_gate_replay'))
    ap.add_argument('--examples', type=int, default=1, help='examples to print per (mutation, status)')
    args = ap.parse_args()
    pairs = load_pairs(args.runs)
    runs = len({p[0] for p in pairs})
    table, examples = measure(pairs, args.binary)
    print(f'{len(pairs)} spans from {runs} runs; verifier {args.binary}')
    print(f'{"family":13} {"mutation":36} {"applied":>7} {"verified":>8} {"prefix":>7} {"rejected":>8} {"short":>6}')
    for (family, name), row in table.items():
        applied = sum(row.values())
        print(f'{family:13} {name:36} {applied:7} {row["verified"]:8} {row["verified_prefix"]:7} {row["rejected_not_in_unit"]:8} '
              f'{row["too_short"]:6}')
    print()
    for (family, name, status), (run, span, mutated, verified) in examples.items():
        if family == 'unchanged' or status == 'too_short':
            continue
        if family == 'orthographic' and status == 'verified':
            continue
        print(f'{family} / {name} -> {status} ({run})')
        print(f'   span     {span[:110]!r}')
        print(f'   mutated  {mutated[:110]!r}')
        if verified is not None:
            print(f'   verified {verified[:110]!r}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
