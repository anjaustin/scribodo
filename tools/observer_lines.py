"""Reads observer_replay's finding lines for the GAP-001 tools (observer_score.py, observer_sample.py).

A finding is twelve pipe-separated fields: type, subject, relation, object, status, content, source kind, doc or chat,
start, len, quote, related text; a newline, carriage return, tab, pipe or backslash inside a text field is escaped as
\\n, \\r, \\t, \\| or \\\\ (observer_replay.c since 2026-11-13). Files written before the escaping can hold one finding
across several physical lines where the real model wrote a subject or content with a newline in it: a record is
complete when it splits into twelve fields with integers in the start and len columns, else the next physical line is
joined to it with a newline. The counts of physical lines, records, joined records and leftover fragments are returned
beside the rows so a measurement discloses them.

The format is src/llm/observer_formats.h's. Since 2026-11-15 the replay declares it on stderr (FORMAT_MARKER, in the
.err beside the .txt, never on stdout, which stays byte-comparable with older runs); read() refuses a declared format it
does not know and reports the declared one, or None for a run before the marker, in stats["format"]."""
import os
import re

FORMAT = 2
FORMAT_MARKER = re.compile(r"^# observer-findings format (\d+)$", re.M)

FIELDS = ["type", "subject", "relation", "object", "status", "content", "kind", "doc", "start", "len", "quote", "related"]
UNESCAPE = {"n": "\n", "r": "\r", "t": "\t", "|": "|", "\\": "\\"}


def split_fields(text):
    """Splits on unescaped pipes and unescapes each field."""
    fields, cur, i = [], [], 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text) and text[i + 1] in UNESCAPE:
            cur.append(UNESCAPE[text[i + 1]])
            i += 2
            continue
        if c == "|":
            fields.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    fields.append("".join(cur))
    return fields


def complete(parts):
    """Twelve fields, integers in start and len, and a type without a newline (a fragment joined in front of a record
    would otherwise read as its type)."""
    return len(parts) == len(FIELDS) and parts[8].strip().isdigit() and parts[9].strip().isdigit() and "\n" not in parts[0]


def format_of(path):
    """The format a replay run declared on its stderr: the .err beside a .txt, or None (no .err, or a run before the
    marker)."""
    err = path[:-4] + ".err" if path.endswith(".txt") else None
    if not err or not os.path.exists(err):
        return None
    m = FORMAT_MARKER.search(open(err, encoding="utf-8", errors="replace").read())
    return int(m.group(1)) if m else None


def read(path):
    """Returns (rows, stats): rows are dicts over FIELDS (start and len as ints) plus 'line', the record's text as read
    (physical lines joined with a newline); stats has lines, records, joined, malformed (physical lines that belong to
    no record). The pending physical lines are kept apart: a record may begin at any of them, so a fragment in front
    of a record is dropped and counted rather than swallowing it (red-team 2026-11-14, second pass)."""
    declared = format_of(path)
    if declared is not None and declared != FORMAT:
        raise ValueError(f"{path}: finding lines format {declared}; this reader knows {FORMAT}")
    rows, stats = [], {"lines": 0, "records": 0, "joined": 0, "malformed": 0, "format": declared}
    pending = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        stats["lines"] += 1
        if not pending and (not line or line.startswith("#")):
            continue
        pending.append(line)
        for start in range(len(pending)):
            text = "\n".join(pending[start:])
            parts = split_fields(text)
            if complete(parts):
                row = dict(zip(FIELDS, parts))
                row["start"], row["len"], row["line"] = int(parts[8]), int(parts[9]), text
                rows.append(row)
                stats["records"] += 1
                stats["joined"] += len(pending) - start > 1
                stats["malformed"] += start
                pending = []
                break
        else:
            # Nothing complete yet: drop leading lines while the join already holds more than twelve fields, since
            # no later line can mend that; what remains may still be the head of a record written across lines.
            while pending and len(split_fields("\n".join(pending))) > len(FIELDS):
                pending.pop(0)
                stats["malformed"] += 1
    stats["malformed"] += len(pending)
    return rows, stats


def stats_line(stats):
    return "LINES physical={lines} records={records} joined={joined} malformed={malformed}".format(**stats)
