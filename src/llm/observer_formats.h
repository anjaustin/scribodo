/* The observer's two C-to-Python contracts (AUDIT-004 F7), defined here once, as llm/answer_receipt.h defines the
 * receipt. The C test (tests/test_observer_formats.c) writes every kind of line into tests/fixtures/observer_formats/
 * and the Python test (tests/test_observer_formats.py) reads those same files with the tools' own readers; change a
 * format here, bump its version, regenerate the fixtures (SCRIBODO_WRITE_FIXTURES=1 ./build/test_observer_formats, from
 * the repository root) and both tests say what else must follow.
 *
 * 1. The judge log (observer_set_judge_log; tools/observer_judge_score.py load()). One JSON object per line, one line
 *    per judge call, in this member order:
 *      "version"    OBSERVER_JUDGE_LOG_VERSION. Logs written before 2026-11-15 have no "version" and are version 0:
 *                   the same members without this one. A reader refuses a version it does not know.
 *      "kind"       "doc" or "chat"
 *      "doc"        the document's name, or the chat's id
 *      "message"    the chat message's index (0 for a document)
 *      "start"      the statement's source span: first character, in code points of the document or the message
 *      "len"        its length in code points
 *      "type"       the finding type of the statement; "sentence" for a source sentence
 *      "path"       "statement" (the model's statement) or "sentence" (a source sentence, SCRIBODO_OBSERVER_SENTENCES)
 *      "attributes" only on the sentence path: obs_attributes_differ(canon, statement)
 *      "subject"    only on the sentence path: obs_same_subject(canon, statement)
 *      "canon_id"   the canon entry's record id
 *      "canon"      the canon entry's text
 *      "statement"  the text the judge saw
 *      "replies"    the two raw judge replies, each a string or null (the second is null when the first vote was not
 *                   a conflict)
 *      "conflict"   the judge's two votes, before any co-signer and any dedupe
 *
 * 2. The replay's finding lines (tools/observer_replay.c stdout; tools/observer_lines.py read()). One line per finding,
 *    sorted, twelve fields separated by "|": type, subject, relation, object, status, content, source kind ("doc",
 *    "chat" or empty), document name or chat id, start, len (the first source's span in code points, decimal), quote,
 *    related text. In a text field a newline, carriage return, tab, pipe or backslash is written \n, \r, \t, \| or \\.
 *    OBSERVER_FINDING_LINES_FORMAT is 2 since 2026-11-13 (format 1 wrote text fields raw, so a finding could span
 *    lines). The format is declared on the replay's stderr as the line OBSERVER_FINDING_LINES_MARKER, never on stdout:
 *    stdout stays byte-comparable with every run before the marker (the fake-path yardstick). The reader looks for it
 *    in the .err beside the .txt; a run without it predates the marker and is read by the joining rules that handle
 *    format 1 and 2 alike. */
#ifndef SCRIBODO_OBSERVER_FORMATS_H
#define SCRIBODO_OBSERVER_FORMATS_H

#include <stdbool.h>
#include <stddef.h>

#include "app/records.h"

#define OBSERVER_JUDGE_LOG_VERSION 1
#define OBSERVER_FINDING_LINES_FORMAT 2
#define OBSERVER_FINDING_LINES_MARKER "# observer-findings format 2"

typedef struct {
    bool is_doc;
    const char *name;      /* document name or chat id */
    long message;          /* SourceRef.message */
    size_t start, len;
    const char *type;
    bool sentence, attributes, subject;
    const char *canon_id, *canon, *statement;
    const char *replies[2]; /* NULL is written as null */
    bool conflict;
} ObserverJudgeLogEntry;

/* One judge-log line, without its newline; malloc'd, NULL on allocation failure. */
char *observer_judge_log_line(const ObserverJudgeLogEntry *e);

/* One finding line of the replay, without its newline; malloc'd, NULL on allocation failure. */
char *observer_finding_line(const RecordVersion *v);

#endif
