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


# The C normalization's alphabet (answer_normalize, src/llm/answer_pipeline.c): ASCII letters and digits are the word
# characters (C isalnum in the C locale), ASCII whitespace plus the Unicode spaces unicode_space_at lists are blanks,
# and only ASCII letters fold case (C tolower).
ALNUM = frozenset('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789')
BLANKS = frozenset(' \t\n\v\f\r\u00a0\u2028\u2029\u202f\u205f\u3000\u2013\u2014-') | frozenset(chr(c) for c in range(0x2000, 0x200b))
LOWER = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')


def normalize(text):
    """Fold typographic variants so a quotation is judged on words, not glyphs: a character-for-character port of the
    C normalization, kept equal to it by a differential fuzz (journal/redteam-loose_ends.md). Curly quotes become
    straight, dashes and Unicode spaces become one blank, a Markdown marker folds at a word's edge and stays between
    two word characters (har_dly), double quotes go, a single quote stays only as an apostrophe inside a word."""
    out = []
    space = True  # leading whitespace is dropped
    n = len(text)
    i = 0
    while i < n:
        ch = text[i]
        c = None
        if ch in BLANKS:
            blank = True
        else:
            blank = False
            if ch in '\u201c\u201d':
                c = '"'
            elif ch in '\u2018\u2019':
                c = "'"
            elif ch in '_*' and not (i > 0 and text[i - 1] in ALNUM and i + 1 < n and text[i + 1] in ALNUM):
                i += 1
                continue
            else:
                c = ch.translate(LOWER)
        if c == '"':
            i += 1
            continue
        if c == "'" and not (out and out[-1] in ALNUM and i + 1 < n and text[i + 1] in ALNUM):
            i += 1
            continue
        if blank:
            if not space:
                out.append(' ')
                space = True
        else:
            out.append(c)
            space = False
        i += 1
    while out and out[-1] == ' ':
        out.pop()
    return ''.join(out)


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
