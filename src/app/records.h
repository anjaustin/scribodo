/* Notes and canon (SOW §4.4): versioned project records.
 *
 *   <project>/.scribodo/notes.jsonl   <project>/.scribodo/canon.jsonl
 *   <project>/.scribodo/findings.jsonl  (observer findings)
 *
 * Each line is one complete version of one record:
 *
 *   {"id":"c-…","ver":2,"ts":…,"status":"committed","content":"…",
 *    "sources":[{"kind":"doc","doc":"ch1.md","revision":"…-source.md",
 *                "start":120,"len":42,"quote":"…"}]}
 *
 * The log is append-only: the newest version of an id is its current state,
 * and every earlier version stays readable, so canon revisions are always
 * traceable. Lines are written with one write() each and fsync'd; a torn
 * line from a crash is skipped. Interim storage until the MariaDB-backed
 * project store (SOW §5).
 *
 * Notes are editable, non-authoritative records (status "active" or
 * "deleted"). Canon entries are explicit, author-approved assertions
 * (status "committed" or "retracted"); nothing becomes canon except through
 * records_append() called for an explicit author action.
 */
#ifndef SCRIBODO_RECORDS_H
#define SCRIBODO_RECORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "app/store.h"

typedef enum { REC_NOTE = 0, REC_CANON = 1, REC_FINDING = 2 } RecordKind;
typedef enum { SRC_DOC = 0, SRC_CHAT = 1 } SourceKind;

/* Where a record came from. A document source names an immutable revision
 * snapshot holding the text, plus the range (codepoints) and the quote, so
 * provenance survives later edits. A chat source names a message. */
typedef struct {
    SourceKind kind;
    char doc[256];       /* document file name within the project */
    char revision[160];  /* revision snapshot file (see revisions.h) */
    char chat[40];       /* chat id */
    long message;        /* message index within the chat */
    size_t start, len;   /* range within the revision text or the message */
    char *quote;         /* the referenced text (UTF-8) */
} SourceRef;

/* Observer findings (REC_FINDING) carry a type and, for relationships, a
 * typed link; conflicts and overlaps name the related canon entry or note.
 * Status: "new", "saved", "committed" or "dismissed". */
typedef struct {
    char type[24];      /* entity, event, relationship, question, conflict */
    char subject[80], relation[48], object[80];
    char related[40];   /* id of the canon entry (conflict) or note (overlap) */
    char *related_text; /* that entry's text when the finding was made */
} FindingInfo;

typedef struct {
    int ver;
    time_t ts;
    char status[16];
    char *content;
    SourceRef *sources;
    size_t nsources;
    FindingInfo finding; /* REC_FINDING only */
} RecordVersion;

typedef struct {
    char id[40];
    RecordVersion *versions; /* oldest first; versions[nversions-1] is current */
    size_t nversions;
    size_t created_seq;      /* order of creation in the log (higher = newer) */
} Record;

static inline const RecordVersion *record_current(const Record *r) { return &r->versions[r->nversions - 1]; }

/* New id: "n-YYYYMMDD-HHMMSS-xxxx" (note), "c-…" (canon) or "f-…" (finding). */
void records_new_id(RecordKind kind, char *out, size_t outlen);
/* As records_new_id, but never an id in `taken` (the log the writer has just read under its lock): the count moves
 * past any id already there, so a new record cannot land on an existing one and hide it as its next version. */
void records_new_id_unused(RecordKind kind, const Record *taken, size_t ntaken, char *out, size_t outlen);

/* Append a new version of record `id` (its number is one past the newest).
 * Returns false on I/O failure or an invalid id; *ver_out gets the number. */
bool records_append(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                    const SourceRef *sources, size_t nsources, int *ver_out);
/* As records_append, with finding details (NULL for notes and canon). */
bool records_append_ex(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                       const SourceRef *sources, size_t nsources, const FindingInfo *finding, int *ver_out);

/* A new record (version 1) under an id just made by records_new_id(),
 * without reading the log. */
bool records_create_ex(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                       const SourceRef *sources, size_t nsources, const FindingInfo *finding);

/* All records, most recently changed first. Caller frees with records_free. */
size_t records_list(StoreManager *manager, const char *project_dir, RecordKind kind, Record **out);
bool records_list_checked(StoreManager *manager, const char *project_dir, RecordKind kind, Record **out, size_t *count);
/* The file store's records, whatever the project's mode (the importer). */
size_t records_list_files(const char *project_dir, RecordKind kind, Record **out);
void records_free(Record *r, size_t n);

/* Deep copy of a list of sources (quote strings included). */
SourceRef *records_copy_sources(const SourceRef *s, size_t n);
void records_free_sources(SourceRef *s, size_t n);

#endif
