#!/usr/bin/env python3
"""Relation arm: can a deterministic check hold a relation in the reply to the sources? Diagnostic only; no model.

Every wrong statement shown or withheld on either novel at the current tree is a relation between sourced words
("sons" for "daughters", "her father is Tardos Mors", two draft labels swapped): the claim gate reads names and
numbers, the quotation check reads quoted text, and neither reads a relation the composer wrote in its own words
(journal/answer-cycle4_window.md, decisions 1). This arm measures one candidate check on every captured reply:

  relation phrases   kinship and possession phrases in the reply's own prose (outside quotations and block quotes):
                     "<kin> of <Name>", "<Name>'s <noun>", "<pronoun or Name> <kin> is <Name>", "<Name> is the <kin>
                     of <Name>", where kin is a closed list (father, mother, son, daughter, sister, brother, wife,
                     husband, friend, estate, fortune, inheritance, property) and Name is a capitalized
                     run (honorific allowed);
  support            a phrase is supported when its normalized text (the citation classifier's folding: quotes,
                     dashes, emphasis, case, whitespace) is a substring of a verified span, an author turn or the
                     question; "X's Y" is also supported by "Y of X";
  negation           a sentence carrying "not", "never", "neither", "nor", "wrong" or "instead" is skipped: a
                     correct correction states the wrong relation to deny it.

The arm reads the capture directories of live runs (request-N.json for the verified block and the author's turns,
content-N.txt for the reply) and reports, per reply, the phrases found, the unsupported ones and the gate's
outcome, then the tallies: replies flagged, and which. The judgment of each reply (right, wrong, withheld) is the
journals'; this tool says what the check would have flagged, so the two can be compared by hand.
"""
import argparse
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answer_citation_binding import normalize  # noqa: E402

KIN = r"(?:father|mother|son|daughter|sister|sisters|brother|brothers|wife|husband|friend|estate|fortune|inheritance|property|nephew|niece|cousin|cousins|uncle|aunt|grandfather|grandmother|grandson|granddaughter|widow|heir)"
NAME = r"(?:(?:Mr\.|Mrs\.|Miss|Dr\.|Sir|Lady) )?[A-Z][a-z]+(?: [A-Z][a-z]+)*"
PHRASES = [
    re.compile(rf"\b({KIN}) of ({NAME})"),
    re.compile(rf"\b({NAME})['’]s ({KIN})\b"),
    # The subject is a pronoun or a possessive name: "the youngest Bennet daughter is Lydia" (a family name as an
    # adjective) is not a relation statement; "Lydia's father is Mr. Bennet" is one, checked whole.
    # "is" and "was" alike: a relation stated in the past tense is stated (journal/redteam-loose_ends.md).
    re.compile(rf"\b([Hh]is|[Hh]er|[Tt]heir|{NAME}['’]s) ({KIN}) (?:is|was) (?:the |a )?({NAME})"),
    re.compile(rf"\b({NAME}) (?:is|was) (?:the |a |not the )?({KIN}) of ({NAME})"),
]
NEGATION = re.compile(r"\b(?:not|never|neither|nor|wrong|instead|isn't|aren't|wasn't)\b", re.I)
# A negation reaches its own clause: "Hareton is Heathcliff's nephew, not his son" states the first and denies the second.
CLAUSE_BREAK = re.compile(r'[,;:]\s+|\s+but\s+')


def clause_bounds(s):
    bounds, start = [], 0
    for m in CLAUSE_BREAK.finditer(s):
        bounds.append((start, m.start()))
        start = m.end()
    bounds.append((start, len(s)))
    return bounds


def negated_at(s, bounds, pos):
    for a, b in bounds:
        if a <= pos < b:
            return bool(NEGATION.search(s[a:b]))
    return bool(NEGATION.search(s))
SPAN = re.compile(r'Source (\d+) \(([^)]+)\): "(.*?)"\n', re.S)


UNICODE_SPACES = re.compile('[\u00a0\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]')  # the C's unicode_space_at


def prose(reply):
    """The reply outside quotations and block quotes (the quotation check's territory). Unicode spaces are plain
    spaces, as the C's rel_prose reads them, so the patterns' literal spaces match a no-break space."""
    out = []
    for line in UNICODE_SPACES.sub(' ', reply).split('\n'):
        if line.lstrip().startswith('>'):
            continue
        line = re.sub(r'"[^"\n]*"', ' ', line)
        line = re.sub(r'“[^”\n]*”', ' ', line)
        out.append(line)
    return '\n'.join(out)


SENTENCE_END = re.compile(r'(?<!\bMr\.)(?<!\bMrs\.)(?<!\bDr\.)(?<!\bSt\.)(?<=[.!?])\s+|\n+')


def sentences(text):
    """Sentences of the prose; an honorific's period ("Mr. Bingley") does not end one."""
    return [s for s in SENTENCE_END.split(text) if s.strip()]


# Capitalized words that are not names: sentence starters and labels the composer writes.
NOT_NAMES = {'The', 'This', 'That', 'These', 'Those', 'It', 'So', 'If', 'Let', 'However', 'According', 'From', 'Based',
             'Source', 'Sources', 'Summary', 'Answer', 'Current', 'Older', 'Correction', 'Note', 'Verified', 'Neither',
             'Both', 'Also', 'And', 'But', 'In', 'On', 'For', 'With', 'Your', 'His', 'Her', 'Their', 'My',
             'She', 'He', 'They', 'We', 'You', 'Who', 'Whose', 'What', 'Which', 'When', 'Where', 'Why', 'How', 'Yes', 'No',
             'Here', 'There', 'Then', 'Thus', 'Now'}


def real_name(name):
    words = name.split()
    if words[0].rstrip('.') in ('Mr', 'Mrs', 'Miss', 'Dr', 'Sir', 'Lady') and len(words) > 1:
        words = words[1:]
    return words[0] not in NOT_NAMES


def phrases(reply):
    """(phrase text, negated) for every relation phrase in the reply's prose whose name is a name."""
    found = []
    for s in sentences(prose(reply)):
        if '?' in s:
            continue  # a question the composer asks back states nothing
        bounds = clause_bounds(s)
        for rx in PHRASES:
            for m in rx.finditer(s):
                if rx is not PHRASES[1] and re.match(r"['’]s", s[m.end():]):
                    continue  # "daughter of Mr. Bingley's cousin" is not "daughter of Mr. Bingley": the name goes on
                negated = negated_at(s, bounds, m.start())
                names = [re.sub(r"['’]s$", '', g) for g in m.groups() if g and re.match(r'^(?:(?:Mr\.|Mrs\.|Miss|Dr\.|Sir|Lady) )?[A-Z]', g)
                         and not re.match(KIN + '$', g) and g.lower() not in ('his', 'her', 'their')]
                if names and all(real_name(n) for n in names):
                    found.append((m.group(0), negated))
    return found


def supported(phrase, sources):
    n = normalize(phrase)
    if any(n in src for src in sources):
        return True
    m = re.match(rf"({NAME})['’]s ({KIN})$", phrase)
    if m and any(normalize(f"{m.group(2)} of {m.group(1)}") in src for src in sources):
        return True
    # The one possession word with a verb: "X's inheritance" is also supported by "X inherited" (the C check's rule).
    if m and m.group(2) == 'inheritance' and any(normalize(f"{m.group(1)} inherited") in src for src in sources):
        return True
    return False


POSSESSION = {'estate', 'fortune', 'inheritance', 'property'}


def phrase_class(phrase):
    """'possession' when the phrase's relation word is a possession, else 'kinship'."""
    words = re.findall(KIN, phrase)
    return 'possession' if words and words[-1] in POSSESSION else 'kinship'


def check(reply, spans, turns, question):
    sources = [normalize(t) for t in spans + turns + [question]]
    rows = []
    for text, negated in phrases(reply):
        rows.append({'phrase': text, 'class': phrase_class(text), 'negated': negated, 'supported': supported(text, sources),
                     'flag': not negated and not supported(text, sources)})
    return rows


def visible(reply):
    """What the product shows and checks (llm_split_thinking): a reply that starts with <think> is reasoning up to
    </think>; an orphan </think> means everything before it was reasoning; any other <think> is ordinary text."""
    lead = reply.lstrip(' \n\r\t')
    if lead.startswith('<think>'):
        body = lead[len('<think>'):]
        rest = body.split('</think>', 1)[1] if '</think>' in body else ''
    else:
        head, sep, tail = reply.partition('</think>')
        rest = tail if sep else reply
    return rest.lstrip(' \n\r\t')


def load_capture(d):
    out = []
    for i in range(100):
        rq = d / f'request-{i}.json'
        if not rq.exists():
            break
        r = json.loads(rq.read_text())
        system = r['messages'][0]['content']
        spans = [t for _, _, t in SPAN.findall(system)]
        turns = [m['content'] for m in r['messages'][1:] if m['role'] == 'user']
        question = turns[-1] if turns else ''
        reply_path = d / f'content-{i}.txt'
        reply = visible(reply_path.read_text()) if reply_path.exists() else ''
        out.append({'index': i, 'question': question, 'spans': spans, 'turns': turns[:-1], 'reply': reply})
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--capture', action='append', type=Path, required=True, help='a capture directory; repeatable')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, help='also write the replies with their sources and this check\'s result as the C parity fixture')
    args = parser.parse_args()
    results, flagged, total, fixture = [], 0, 0, []
    by_class = {}  # (class, outcome) -> phrases; the kinship/possession split (journal/answer-cycle4_relation_split.md)
    only_by = {'kinship': 0, 'possession': 0, 'both': 0}
    for d in args.capture:
        for row in load_capture(d):
            rows = check(row['reply'], row['spans'], row['turns'], row['question'])
            total += 1
            flags = [r for r in rows if r['flag']]
            flagged += bool(flags)
            for r in rows:
                key = (r['class'], 'flag' if r['flag'] else 'negated' if r['negated'] else 'supported')
                by_class[key] = by_class.get(key, 0) + 1
            classes = {r['class'] for r in flags}
            if classes:
                only_by['both' if len(classes) == 2 else classes.pop()] += 1
            results.append({'capture': d.name, **{k: row[k] for k in ('index', 'question')}, 'phrases': rows, 'flagged': bool(flags)})
            fixture.append({'capture': d.name, 'index': row['index'], 'question': row['question'], 'spans': row['spans'], 'turns': row['turns'],
                            'reply': row['reply'], 'checked': len(rows), 'unsupported': flags[0]['phrase'] if flags else ''})
            mark = 'FLAG' if flags else '    '
            print(f"{mark} {d.name:22s} #{row['index']} {row['question'][:60]!r}")
            for r in rows:
                print(f"       {'flag' if r['flag'] else 'neg ' if r['negated'] else 'ok  '} {r['phrase']}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'replies': total, 'flagged': flagged, 'results': results}, indent=2, ensure_ascii=False) + '\n')
    if args.fixture:
        args.fixture.parent.mkdir(parents=True, exist_ok=True)
        args.fixture.write_text(json.dumps({'purpose': 'Recorded composer replies with their sources and the relation check\'s result '
                                            '(tools/answer_relation_arm.py); the C check (answer_relations) must agree on every row.',
                                            'replies': fixture}, indent=1, ensure_ascii=False) + '\n')
    print(f"\n{flagged} of {total} replies flagged; {args.output}")
    for cls in ('kinship', 'possession'):
        print(f"  {cls:10} phrases: {by_class.get((cls, 'supported'), 0)} supported, {by_class.get((cls, 'negated'), 0)} negated, "
              f"{by_class.get((cls, 'flag'), 0)} flagged")
    print(f"  replies flagged by a kinship phrase only {only_by['kinship']}, by a possession phrase only {only_by['possession']}, by both {only_by['both']}")


if __name__ == '__main__':
    main()
