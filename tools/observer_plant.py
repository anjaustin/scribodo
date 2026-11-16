#!/usr/bin/env python3
"""Plant the committed continuity faults into a copy of a novel, for the observer's recall measurement (GAP-001).

usage: tools/observer_plant.py FAULTS.json SRC_DIR DST_DIR

Each fault names a file, an anchor sentence that must occur exactly once in it, and a sentence to insert right after
the anchor (the original text is kept whole: a planted fault is an addition that contradicts a fact stated elsewhere).
Writes DST_DIR/<file> for every .md in SRC_DIR, DST_DIR/faults-applied.json with the offset and length of each
inserted sentence in its file in bytes (offset, length: for slicing the file) and in characters (char_offset,
char_length: the unit of the observer's source spans, which the scorer compares against), and prints the `--canon` arguments the replay needs (one per distinct canon statement),
so the judge path has the original facts to compare against. Exit 2 if any anchor is missing, ambiguous or stops short of a sentence's end: the list
is then wrong and nothing is written. An anchor must also end a sentence: one that stops at a title or an initial
("... at Mr.") would put the insert inside a sentence ("at Mr. Mr. Collins, who was ..."), as two Pride and
Prejudice anchors did until 2026-11-15 (journal/observer-cycle2_quality.md, step 7)."""
import json
import os
import re
import shutil
import sys

ABBREVIATIONS = {"mr", "mrs", "ms", "dr", "st", "capt", "col", "lt", "gen", "prof", "sgt", "jr", "sr", "rev", "hon"}


# A single capital is a name's initial only in context (name_initial): the lists and the rule of
# observer_text.c name_initial, itself the data/text stream's text_name_initial_before (red-team 2026-11-15).
LABEL_WORDS = {"Chapter", "Ch", "Part", "Book", "Volume", "Vol", "Act", "Scene", "Section", "Sec", "Source", "Appendix",
               "Figure", "Fig", "Table", "Plate", "Page", "Item", "Note", "Step", "Letter", "Line", "Option", "Plan",
               "Type", "Class", "Group", "Exhibit", "Article", "Clause"}
SENTENCE_STARTERS = {"The", "This", "That", "These", "Those", "It", "Its", "He", "She", "They", "We", "You", "I", "His",
                     "Her", "Their", "My", "Our", "There", "Here", "Then", "Thus", "So", "But", "And", "Or", "If", "When",
                     "While", "As", "In", "On", "At", "For", "With", "From", "According", "However", "Also", "Yes", "No",
                     "Now", "Who", "What", "Which", "Where", "A"}
_SPACE = " \t\n\r\v\f"


def _letter(c):
    return "A" <= c <= "Z" or "a" <= c <= "z"


def name_initial(text, dot):
    """True when the single capital before text[dot] (a period) is a name's initial: not "I", no word character right
    before it, the word after it capitalised and not a usual sentence starter ("said I. Proud", "Mr. X. Then"), and the
    word before it, if any, not a label ("Chapter V.", "Part I."). "J. R. R. Tolkien", "T. S. Eliot" at the start and
    "names E. Bennet" keep their initials. observer_text.c name_initial, character for character (the verifier's rule,
    2026-11-15)."""
    if dot < 1 or dot >= len(text) or not "A" <= text[dot - 1] <= "Z" or text[dot - 1] == "I":
        return False
    if dot >= 2 and (_letter(text[dot - 2]) or "0" <= text[dot - 2] <= "9" or text[dot - 2] == "_"):
        return False
    j = dot + 1
    while j < len(text) and text[j] in _SPACE:
        j += 1
    if j >= len(text) or not "A" <= text[j] <= "Z":
        return False
    je = j
    while je < len(text) and _letter(text[je]):
        je += 1
    if text[j:je] in SENTENCE_STARTERS:
        return False
    e = dot - 1
    while e > 0 and text[e - 1] not in _SPACE:
        e -= 1
    while e > 0 and text[e - 1] in _SPACE:
        e -= 1
    b = e
    while b > 0 and text[b - 1] not in _SPACE:
        b -= 1
    while b < e and not _letter(text[b]):
        b += 1
    while e > b and not _letter(text[e - 1]):
        e -= 1
    return e <= b or text[b:e] not in LABEL_WORDS


def ends_sentence(anchor, after):
    """True when the anchor ends with sentence punctuation (closing quotes allowed), what follows in the text is a
    space, a line end or the end of the file, and a final period does not close a title or an initial: the letters
    right before it are not on ABBREVIATIONS, and a single capital is not a name's initial in context (name_initial,
    which reads the next word in `after`: pass the rest of the line, not one character). "drily--\u201cMr." is a
    title, "Kan\u2019s." and "in Chapter V." end a sentence. This checks an anchor's end only. obs_sentence_end in
    src/llm/observer_text.c, which cuts running text, agrees on titles, initials and closing quotes but also requires a
    capital (an opening quote allowed) after the space, which is observer_sentence_pairs.BOUNDARY's rule: "ends with i.
    next" ends a sentence here and none there. tests/fixtures/observer_rules/cases.json holds the C splitter and
    BOUNDARY with this check to the same sentences."""
    m = re.search(r"([A-Za-z]*)([.!?])[\"'\u201d\u2019)]*$", anchor)
    if not m or (after and not after[0].isspace()):
        return False
    word = m.group(1)
    if m.group(2) != ".":
        return True
    if word.lower() in ABBREVIATIONS:
        return False
    return not (len(word) == 1 and name_initial(anchor + after, m.start(2)))


def plan(faults, src):
    texts, planned, errors = {}, [], []
    for f in faults:
        path = os.path.join(src, f["file"])
        if f["file"] not in texts:
            try:
                texts[f["file"]] = open(path, encoding="utf-8").read()
            except OSError as e:
                errors.append(f"{f['id']}: cannot read {path}: {e}")
                continue
        n = texts[f["file"]].count(f["anchor"])
        if n != 1:
            errors.append(f"{f['id']}: anchor occurs {n} times in {f['file']}, needs exactly 1")
        else:
            text = texts[f["file"]]
            end = text.index(f["anchor"]) + len(f["anchor"])
            if not ends_sentence(f["anchor"], text[end:end + 200]):
                errors.append(f"{f['id']}: anchor does not end a sentence in {f['file']} (it ends at "
                              f"{f['anchor'][-12:]!r}); the insert would land inside one")
            continue
        text = texts[f["file"]]
        end = text.index(f["anchor"]) + len(f["anchor"])
        if not ends_sentence(f["anchor"], text[end:end + 200]):
            errors.append(f"{f['id']}: anchor does not end a sentence in {f['file']} (it stops at "
                          f"{f['anchor'][-12:]!r}); the insert would land inside one")
    return texts, errors


def apply(faults, texts):
    """Insert in one forward pass per file, in order of anchor position (list order at a shared anchor). offset and
    length are bytes of UTF-8; char_offset and char_length are code points, the unit of the observer's source spans
    (SourceRef start, len: measured 2026-11-13, every real-model finding slices to its quote by characters)."""
    applied = []
    by_file = {}
    for f in faults:
        by_file.setdefault(f["file"], []).append(f)
    out = dict(texts)
    for name, fs in by_file.items():
        data = texts[name].encode("utf-8")
        order = sorted(enumerate(fs), key=lambda e: (data.index(e[1]["anchor"].encode("utf-8")) + len(e[1]["anchor"].encode("utf-8")), e[0]))
        pieces, last = [], 0
        for _, f in order:
            anchor = f["anchor"].encode("utf-8")
            end = data.index(anchor) + len(anchor)
            if end < last:  # a second fault at the same anchor: it follows the first insertion
                end = last
            pieces.append(data[last:end])
            insert = f["insert"].encode("utf-8")
            offset = sum(len(x) for x in pieces) + 1  # after the space
            pieces.append(b" " + insert)
            applied.append({"id": f["id"], "type": f["type"], "file": name, "insert": f["insert"], "canon": f["canon"],
                            "offset": offset, "length": len(insert)})
            last = end
        pieces.append(data[last:])
        joined = b"".join(pieces)
        out[name] = joined.decode("utf-8")
        for a in applied:
            if a["file"] == name:
                a["char_offset"] = len(joined[:a["offset"]].decode("utf-8"))
                a["char_length"] = len(a["insert"])
    applied.sort(key=lambda a: (a["file"], a["offset"]))
    return out, applied


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 1
    spec = json.load(open(sys.argv[1], encoding="utf-8"))
    src, dst = sys.argv[2], sys.argv[3]
    faults = spec["faults"]
    texts, errors = plan(faults, src)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 2
    for name in sorted(os.listdir(src)):
        if name.endswith(".md") and name not in texts:
            texts[name] = open(os.path.join(src, name), encoding="utf-8").read()
    out, applied = apply(faults, texts)
    os.makedirs(dst, exist_ok=True)
    for name, text in out.items():
        with open(os.path.join(dst, name), "w", encoding="utf-8") as fh:
            fh.write(text)
    for a in applied:  # both recorded offsets must point at the inserted sentence
        data = out[a["file"]].encode("utf-8")
        assert data[a["offset"]:a["offset"] + a["length"]] == a["insert"].encode("utf-8"), a["id"]
        assert out[a["file"]][a["char_offset"]:a["char_offset"] + a["char_length"]] == a["insert"], a["id"]
    with open(os.path.join(dst, "faults-applied.json"), "w", encoding="utf-8") as fh:
        json.dump({"novel": spec.get("novel"), "faults": applied}, fh, indent=1, ensure_ascii=False)
    canon = []
    for f in faults:
        if f["canon"] not in canon:
            canon.append(f["canon"])
    for c in canon:
        print(f"--canon\t{c}")
    print(f"# planted {len(applied)} faults in {len(set(a['file'] for a in applied))} files; {len(canon)} canon statements", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
