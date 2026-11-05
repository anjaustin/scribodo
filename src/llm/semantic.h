/* Semantic retrieval (SOW §5; docs/STORE-001-design.md "Qdrant").
 *
 * Qdrant holds a rebuildable index of a project's text: canon entries,
 * notes, document pieces and chat messages. Each point carries its ids
 * (record, document, revision, conversation, range) and its authority
 * ("canon", "note", "draft" for document text, "chat"), so retrieval can
 * be limited to committed canon and never confuses canon with draft
 * material. MariaDB stays the authority: semantic_rebuild() recreates a
 * project's index from it alone.
 *
 * Vectors come from Qwen3-Embedding-0.6B served by its own llama-server
 * (llm_server_embed_config); tests use a deterministic fake embedder.
 * Projects not in the store have no index (retrieval falls back to the
 * observer's lexical overlap).
 */
#ifndef SCRIBODO_SEMANTIC_H
#define SCRIBODO_SEMANTIC_H

#include <stdbool.h>
#include <stddef.h>

#include "llm/server.h"

#define SEM_DIM 1024
/* The longest text embedded, in bytes (longer ones by their start). The
 * embedding server's context is sized from it (llm_server_embed_config). */
#define SEM_MAX_INPUT_BYTES 2000

/* The embedding server (NULL: the fake embedder). */
void semantic_set_embedder(LlmServer *embed);
/* Whether retrieval is available right now (Qdrant reachable and, for the
 * real embedder, its server ready). */
bool semantic_ready(void);

/* Embed `n` texts into out[n * SEM_DIM] (unit length). Queries get the
 * model's instruction prefix; stored passages don't. */
bool semantic_embed(const char *const *texts, size_t n, bool query, float *out, char *err, size_t errlen);

typedef struct {
    double score;
    char kind[16];      /* canon, note, draft, chat */
    char record_id[40]; /* canon/note */
    char document_id[40], revision_id[40], conversation_id[40];
    long seq;
    size_t start, len;
    char status[16];    /* canon: committed / retracted */
    char *text;         /* the indexed text (malloc'd) */
} SemHit;

void semantic_hits_free(SemHit *h, size_t n);

/* Index (or re-index) the project's current canon and notes from the store.
 * Cheap when nothing changed (versions already indexed are skipped). */
bool semantic_sync_records(const char *project_id, char *err, size_t errlen);
/* Index pieces of a document revision / a chat message (the observer calls
 * these as it reads). */
bool semantic_index_text(const char *project_id, const char *kind, const char *document_id, const char *revision_id,
                         const char *conversation_id, long seq, size_t start, size_t len, const char *text);

/* A document's current pieces (FNV-1a hex of each piece's bytes): points
 * and marks for other pieces of it are removed (superseded text). */
bool semantic_prune_doc(const char *project_id, const char *document_id, const char *const *piece_hashes, size_t n);

/* Nearest committed canon entries to `statement` (at most `limit`). */
size_t semantic_canon_for(const char *project_id, const char *statement, size_t limit, SemHit **out);
/* General search; `kind` NULL for all kinds. */
size_t semantic_search(const char *project_id, const char *query, const char *kind, size_t limit, SemHit **out);

/* Drop the project's index and rebuild it from MariaDB (canon, notes,
 * current document revisions, chat messages). Returns points indexed, or
 * -1 on failure. */
long semantic_rebuild(const char *project_id, char *err, size_t errlen);
/* Drop the project's collection (tests; "wipe"). */
bool semantic_drop(const char *project_id);
/* Tests: lose the collection but keep the marks, as a restore under another
 * database or a wiped Qdrant would. The next sync rebuilds the index. */
bool semantic_forget_collection(const char *project_id);

#endif
