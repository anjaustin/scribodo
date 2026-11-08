/* Deterministic recall over author-committed canon and verified original
 * excerpts (MEMORY-014 P0, MEMORY-015 S1).
 *
 * Nothing on this path calls a generative model, an embedder or the vector
 * index. Matching is lexical, ranking is fixed, every displayed text is the
 * exact stored text, and coverage is reported so that absence is a bounded
 * statement about what was searched, never a claim about the manuscript.
 *
 * Canon is project-wide: the current committed version of every canon entry
 * in the project is searched. Original excerpts respect per-conversation
 * memory consent: only conversations whose memory scope is enabled are
 * searched, and the disabled and still-indexing ones are counted so the
 * author can see what was not covered. Each excerpt is re-verified against
 * its original message or saved revision byte-for-byte before it is shown. */
#ifndef SCRIBODO_RECALL_H
#define SCRIBODO_RECALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "app/records.h"
#include "llm/chat.h"

#define RECALL_QUERY_BYTES 512u
#define RECALL_ROWS_MAX 64u
#define RECALL_CANON_ROWS_MAX 32u
#define RECALL_CONVERSATIONS_MAX 64u
#define RECALL_CANDIDATES_PER_SCOPE 16u

typedef enum { RECALL_CANON = 0, RECALL_CHAT_EXCERPT = 1, RECALL_DOCUMENT_EXCERPT = 2 } RecallRowKind;

typedef struct {
    RecallRowKind kind;
    unsigned matched; /* distinct query terms present: the primary rank */
    double score;     /* excerpts: the SQL full-text score, a secondary rank; canon: 0 */
    char *text;       /* owned exact text: the current committed canon wording, or the verified original excerpt */
    /* Canon */
    char record_id[40];
    int ver;
    time_t ts;
    SourceRef *sources; /* copied from the committed version */
    size_t nsources;
    /* Excerpts */
    char conversation_id[40], conversation_title[256];
    int64_t sequence;
    ChatRole role;
    char document_id[40], revision_id[40], document_name[256];
    bool current_saved;                /* document excerpt: its revision is the document's current saved one */
    size_t codepoint_start, codepoints; /* the range within the original message or revision text */
    uint64_t byte_start;
    size_t text_bytes;
    char unit_id[65];
} RecallRow;

typedef struct {
    RecallRow *rows;
    size_t count;
    unsigned terms;
    size_t canon_committed, canon_retracted; /* canon entries searched, and those excluded by status */
    size_t conversations, memory_on, memory_off, memory_indexing;
    size_t candidates, rejected; /* excerpt candidates, and those that failed exact verification */
    bool canon_ok;               /* the canon list was read; false means nothing authoritative is shown */
    bool excerpts_available;     /* false for file-backed or paused projects and store errors */
    char limits[384];            /* why coverage is bounded, or "" */
} RecallResult;

/* Synchronous. Safe on a worker thread; the caller owns store_thread_done.
 * Returns false only when canon could not be read (the result then has no
 * rows, and limits says why). Excerpt failures are reported in the result. */
bool recall_run(StoreManager *store, const char *project_dir, const char *query, RecallResult *out);
void recall_result_free(RecallResult *result);

/* Deterministic text: one coverage line, then one line per row (owned). */
char *recall_result_text(const RecallResult *result);

#endif
