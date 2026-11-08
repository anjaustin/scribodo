"""Turn a Project Gutenberg plain-text novel into a chapter-per-file manuscript.

    python3 tools/manuscript_import.py SOURCE.txt DEST_DIR [--parts N]

Strips the Gutenberg header and footer, drops [Illustration ...] blocks
(a chapter heading printed inside a caption is kept), splits at chapter
headings ("CHAPTER IV." or "Chapter I" on a line of its own), unwraps the
hard line breaks inside paragraphs and writes chNN.md files with a
"# Chapter N" heading, plus MANUSCRIPT.json with the source, the title and
per-chapter word counts. With --parts N it also writes parts/partNN.md, the
chapters grouped into N consecutive files of about equal size, for a
conversation that can attach at most MEMORY_DOCUMENTS_MAX saved documents.
Deterministic; the text is otherwise unchanged. The destination must not
exist.
"""
import json
import re
import sys
from pathlib import Path

HEADING = re.compile(r"^\s*(?:CHAPTER|Chapter)\s+([IVXLC]+)\.?\]?\s*$")


def chapters(raw):
    text = raw.replace("\r\n", "\n")
    start = text.find("*** START OF THE PROJECT GUTENBERG EBOOK")
    end = text.find("*** END OF THE PROJECT GUTENBERG EBOOK")
    if start < 0 or end < 0:
        raise SystemExit("not a Project Gutenberg text: markers missing")
    body = text[text.index("\n", start) + 1:end]
    # A heading printed inside a caption ("[Illustration: ... Chapter I.]") is a heading.
    body = re.sub(r"\[Illustration:?[^\]]*?((?:CHAPTER|Chapter)\s+[IVXLC]+\.?)\s*\]", r"\n\1\n", body)
    body = re.sub(r"\[Illustration:?[^\]]*\]", "", body)  # captions, possibly multi-line
    title = None
    m = re.search(r"^Title:\s*(.+)$", text[:start], re.M)
    if m:
        title = m.group(1).strip()
    out = []
    current = None
    for line in body.split("\n"):
        h = HEADING.match(line)
        if h:
            current = []
            out.append(current)
            continue
        if current is not None:
            current.append(line)
    if not out:
        raise SystemExit("no chapter headings found")
    result = []
    for lines in out:
        paragraphs = []
        para = []
        for line in lines:
            if line.strip():
                para.append(line.strip())
            elif para:
                paragraphs.append(" ".join(para))
                para = []
        if para:
            paragraphs.append(" ".join(para))
        # The last chapter carries the printer's colophon; drop trailing lines in capitals only.
        while paragraphs and paragraphs[-1].isupper():
            paragraphs.pop()
        result.append(paragraphs)
    return title, result


def main():
    args = sys.argv[1:]
    parts_wanted = 0
    if "--parts" in args:
        i = args.index("--parts")
        parts_wanted = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    source, dest = Path(args[0]), Path(args[1])
    title, parts = chapters(source.read_text(encoding="utf-8"))
    dest.mkdir(parents=True)  # refuse existing destinations
    manifest = {"title": title, "source": source.name, "chapters": []}
    for i, paragraphs in enumerate(parts, 1):
        text = f"# Chapter {i}\n\n" + "\n\n".join(paragraphs) + "\n"
        name = f"ch{i:02}.md"
        (dest / name).write_text(text, encoding="utf-8")
        manifest["chapters"].append({"name": name, "words": len(text.split()), "bytes": len(text.encode("utf-8")),
                                     "paragraphs": len(paragraphs)})
    if parts_wanted:
        (dest / "parts").mkdir()
        total = sum(len(" ".join(p)) for p in parts)
        target = total / parts_wanted
        groups, current, size = [], [], 0
        for i, paragraphs in enumerate(parts, 1):
            current.append(i)
            size += len(" ".join(paragraphs))
            if size >= target * len(groups + [current]) and len(groups) < parts_wanted - 1:
                groups.append(current)
                current = []
        if current:
            groups.append(current)
        manifest["parts"] = []
        for n, group in enumerate(groups, 1):
            text = "".join(f"# Chapter {i}\n\n" + "\n\n".join(parts[i - 1]) + "\n\n" for i in group)
            name = f"part{n:02}.md"
            (dest / "parts" / name).write_text(text, encoding="utf-8")
            manifest["parts"].append({"name": name, "chapters": group, "words": len(text.split()), "bytes": len(text.encode("utf-8"))})
    manifest["words"] = sum(c["words"] for c in manifest["chapters"])
    manifest["bytes"] = sum(c["bytes"] for c in manifest["chapters"])
    (dest / "MANUSCRIPT.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"title": title, "chapters": len(parts), "words": manifest["words"], "bytes": manifest["bytes"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
