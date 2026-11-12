#!/usr/bin/env python3
"""Measure quote-to-source binding in diagnostic replies; no inference.

For every run directory holding <arm>-request.json and <arm>-response.json pairs,
extract quoted spans from the reply (curly or straight quotes, ellipses split a
span into pieces of at least --min-chars), locate which numbered SOURCE block
contains each piece, and compare with the nearest preceding "Source N" label.
Pieces found in no source are unsupported quotations. This is a deterministic
text check, not a judgment of whether the answer is right.
"""
import argparse
import glob
import json
import os
import re

BOUNDARY = "\nAuthor's current request (not part of the retrieved sources):\n"
BLOCK = re.compile(r'--- SOURCE (\d+) \| [^\n]*? ---\n(.*?)\n--- END SOURCE ---', re.S)
# Curly pairs and straight pairs are matched separately so a straight closing
# quote never pairs with a curly opening one across two quotations.
QUOTE = re.compile(r'“([^“”]{12,})”|"([^"]{12,})"')
# Replies write Source, SOURCE and source; the label is the number.
LABEL = re.compile(r'source (\d+)', re.IGNORECASE)


def normalize(text):
    """Fold typographic variants so a quotation is judged on words, not glyphs."""
    text = text.replace('\u201c', '"').replace('\u201d', '"').replace('\u2018', "'").replace('\u2019', "'")
    # Dashes become spaces on both sides so em-dash, spaced-hyphen and hyphen spellings agree.
    text = text.replace('\u2014', ' ').replace('\u2013', ' ').replace('-', ' ')
    text = text.replace('_', '').replace('*', '')  # markdown emphasis inside a quotation is not a wording change
    # Quotation marks are not words (answer_normalize in C): double quotes go; a single quote stays only as an
    # apostrophe inside a word.
    text = text.replace('"', '')
    text = re.sub(r"(?<![A-Za-z0-9])'|'(?![A-Za-z0-9])", '', text)
    return re.sub(r'\s+', ' ', text).strip().lower()


def pieces(span, min_chars):
    return [p.strip(' .,;:') for p in re.split(r'…|\.\.\.', span) if len(p.strip(' .,;:')) >= min_chars]


def classify(user_content, reply, min_chars):
    """Classify each quoted piece of one reply against its own packet."""
    reference, foreground = user_content.split(BOUNDARY) if BOUNDARY in user_content else (user_content, '')
    blocks = {n: normalize(t) for n, t in BLOCK.findall(reference)}
    foreground = normalize(foreground)
    text = reply.rsplit('</think>', 1)[-1]
    counts = {'pieces': 0, 'bound_to_cited_source': 0, 'in_other_source_than_cited': 0,
              'only_in_attachment_preview': 0, 'in_no_supplied_text': 0}
    details = []
    for m in QUOTE.finditer(text):
        label = LABEL.findall(text[:m.start()])
        cited = label[-1] if label else None
        for piece in pieces(m.group(1) or m.group(2), min_chars):
            counts['pieces'] += 1
            piece = normalize(piece)
            holders = [n for n, t in blocks.items() if piece in t]
            if not holders:
                if piece in foreground:
                    counts['only_in_attachment_preview'] += 1
                else:
                    counts['in_no_supplied_text'] += 1; details.append({'piece': piece[:80], 'cited': cited, 'in': []})
            elif cited in holders:
                counts['bound_to_cited_source'] += 1
            else:
                counts['in_other_source_than_cited'] += 1; details.append({'piece': piece[:80], 'cited': cited, 'in': holders})
    return counts, details


def analyze(run, min_chars):
    rows = []
    for req in sorted(glob.glob(os.path.join(run, '*-request.json'))):
        arm = os.path.basename(req)[:-len('-request.json')]
        resp = req.replace('-request.json', '-response.json')
        if not os.path.exists(resp):
            continue
        user = json.load(open(req))['messages'][-1]['content']
        if BOUNDARY not in user:
            continue
        text = json.load(open(resp))['choices'][0]['message'].get('content') or ''
        counts, details = classify(user, text, min_chars)
        rows.append({'run': run, 'arm': arm, **counts, 'details': details})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', nargs='+')
    parser.add_argument('--min-chars', type=int, default=25)
    parser.add_argument('--output')
    args = parser.parse_args()
    rows = [r for run in args.runs for r in analyze(run, args.min_chars)]
    tot = {k: sum(r[k] for r in rows) for k in ('pieces', 'bound_to_cited_source', 'in_other_source_than_cited', 'only_in_attachment_preview', 'in_no_supplied_text')}
    print(json.dumps({'replies': len(rows), 'totals': tot}, indent=2))
    for r in rows:
        if r['in_other_source_than_cited'] or r['in_no_supplied_text']:
            print(f"{r['run']}/{r['arm']}: pieces={r['pieces']} bound={r['bound_to_cited_source']} misbound={r['in_other_source_than_cited']} unsupported={r['in_no_supplied_text']} preview={r['only_in_attachment_preview']}")
    if args.output:
        with open(args.output, 'x') as f:
            json.dump({'min_chars': args.min_chars, 'totals': tot, 'rows': rows}, f, ensure_ascii=False, indent=2); f.write('\n')


if __name__ == '__main__':
    main()
