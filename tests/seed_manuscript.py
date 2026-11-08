"""Disposable manuscript-scale legacy project from the S2 fixture (MEMORY-015 S4).

Writes the fixture's chat archive as a legacy .scribodo chat log and its prose
as chapter documents, so the app's first open moves a whole-manuscript project
into the store and the baseline measures that path. Usage:

    python3 tests/seed_manuscript.py DEST [--words N] [--per-category K] [--seed S] [--manuscript DIR]

With --manuscript DIR the chapter documents are the .md files of a real
manuscript (as written by tools/manuscript_import.py) instead of generated
prose; the chat archive is still the fixture's.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import memory_fixture  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dest", type=Path)
    parser.add_argument("--words", type=int, default=100000)
    parser.add_argument("--per-category", type=int, default=4)
    parser.add_argument("--seed", type=int, default=20261107)
    parser.add_argument("--chapters", type=int, default=8)
    parser.add_argument("--manuscript", type=Path, help="directory of real chapter .md files to use as the documents")
    args = parser.parse_args()
    fixture = memory_fixture.build(args.seed, args.words, args.per_category)
    root = args.dest
    root.mkdir()  # refuse existing destinations
    logs = root / ".scribodo/chats"
    logs.mkdir(parents=True)
    conversation = "20261108-090000-abcd"
    with (logs / f"{conversation}.jsonl").open("w") as stream:
        stream.write(json.dumps({"type": "meta", "version": 1, "title": "Manuscript chat", "created": 1794500000}) + "\n")
        for row in fixture["archive"]:
            stream.write(json.dumps({"type": "message", "role": row["role"], "content": row["text"],
                                     "ts": 1794500000 + row["sequence"], "docs": []}, ensure_ascii=False) + "\n")
    words = 0
    if args.manuscript:
        files = sorted(p for p in args.manuscript.iterdir() if p.suffix == ".md")
        for source in files:
            text = source.read_text(encoding="utf-8")
            (root / source.name).write_text(text, encoding="utf-8")
            words += len(text.split())
        chapters = len(files)
    else:
        prose = [row["text"] for row in fixture["archive"] if row["role"] == "user" and row["sequence"] < fixture["boundary"]]
        per_chapter = max(1, len(prose) // args.chapters)
        for chapter in range(args.chapters):
            paragraphs = prose[chapter * per_chapter:(chapter + 1) * per_chapter]
            text = f"# Chapter {chapter + 1}\n\n" + "\n\n".join(paragraphs) + "\n"
            (root / f"ch{chapter + 1:02}.md").write_text(text)
            words += len(text.split())
        chapters = args.chapters
    manifest = {"seed": args.seed, "words_requested": args.words, "chat_messages": len(fixture["archive"]),
                "chat_words": fixture["words"], "chapters": chapters, "document_words": words,
                "manuscript": str(args.manuscript) if args.manuscript else None}
    (root / "FIXTURE.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
