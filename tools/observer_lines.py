"""Reads observer_replay's finding lines for the GAP-001 tools (observer_score.py, observer_sample.py).

A finding is twelve pipe-separated fields: type, subject, relation, object, status, content, source kind, doc or chat,
start, len, quote, related text; a newline, carriage return, tab, pipe or backslash inside a text field is escaped as
\\n, \\r, \\t, \\| or \\\\ (observer_replay.c since 2026-11-13). Files written before the escaping can hold one finding
across several physical lines where the real model wrote a subject or content with a newline in it: a record is
complete when it splits into twelve fields with integers in the start and len columns, else the next physical line is
joined to it with a newline. The counts of physical lines, records, joined records and leftover fragments are returned
beside the rows so a measurement discloses them."""

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
    return len(parts) == len(FIELDS) and parts[8].strip().isdigit() and parts[9].strip().isdigit()


def read(path):
    """Returns (rows, stats): rows are dicts over FIELDS (start and len as ints) plus 'line', the record's text as read
    (physical lines joined with a newline); stats has lines, records, joined, malformed."""
    rows, stats = [], {"lines": 0, "records": 0, "joined": 0, "malformed": 0}
    buffer, nphys = None, 0
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        stats["lines"] += 1
        if buffer is None:
            if not line or line.startswith("#"):
                continue
            buffer, nphys = line, 1
        else:
            buffer, nphys = buffer + "\n" + line, nphys + 1
        parts = split_fields(buffer)
        if complete(parts):
            row = dict(zip(FIELDS, parts))
            row["start"], row["len"], row["line"] = int(parts[8]), int(parts[9]), buffer
            rows.append(row)
            stats["records"] += 1
            stats["joined"] += nphys > 1
            buffer = None
        elif len(parts) > len(FIELDS):
            stats["malformed"] += 1
            buffer = None
    if buffer is not None:
        stats["malformed"] += 1
    return rows, stats


def stats_line(stats):
    return "LINES physical={lines} records={records} joined={joined} malformed={malformed}".format(**stats)
