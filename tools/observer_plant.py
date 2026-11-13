#!/usr/bin/env python3
"""Plant the committed continuity faults into a copy of a novel, for the observer's recall measurement (GAP-001).

usage: tools/observer_plant.py FAULTS.json SRC_DIR DST_DIR

Each fault names a file, an anchor sentence that must occur exactly once in it, and a sentence to insert right after
the anchor (the original text is kept whole: a planted fault is an addition that contradicts a fact stated elsewhere).
Writes DST_DIR/<file> for every .md in SRC_DIR, DST_DIR/faults-applied.json with the byte offset and length of each
inserted sentence in its file, and prints the `--canon` arguments the replay needs (one per distinct canon statement),
so the judge path has the original facts to compare against. Exit 2 if any anchor is missing or ambiguous: the list
is then wrong and nothing is written."""
import json
import os
import shutil
import sys


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
    return texts, errors


def apply(faults, texts):
    """Insert in one forward pass per file, in order of anchor position (list order at a shared anchor); offsets and
    lengths are bytes of UTF-8, which is how the observer's source spans count (SourceRef start, len)."""
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
        out[name] = b"".join(pieces).decode("utf-8")
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
    for a in applied:  # the recorded byte offset must point at the inserted sentence
        data = out[a["file"]].encode("utf-8")
        assert data[a["offset"]:a["offset"] + a["length"]] == a["insert"].encode("utf-8"), a["id"]
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
