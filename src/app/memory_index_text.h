/* Experiment (journal/answer-cycle3_coref.md): an index layer beside the
 * original unit text.
 *
 * With SCRIBODO_INDEX_TEXT_FILE naming a JSON object of
 * {sha256(original unit text) in lowercase hex: resolved text}, every unit
 * ingested also stores an index_text: the resolved text when the file has one
 * for that unit, else a copy of the original. The lexical queries then match
 * index_text, the vector index embeds it, and the verified pipeline reranks
 * it. The original text stays the stored, hash-verified, extracted and quoted
 * form; index_text is never shown and is rebuilt with the index. With the
 * variable unset nothing changes: index_text is NULL and every reader uses
 * text. A set variable whose file does not load is fatal: an experiment that
 * silently ran unresolved would be a wrong measurement, not a degraded one. */
#ifndef SCRIBODO_MEMORY_INDEX_TEXT_H
#define SCRIBODO_MEMORY_INDEX_TEXT_H

#include <stdbool.h>
#include <stddef.h>

#define MEMORY_INDEX_TEXT_BYTES 8000u /* a resolved unit; the embedder and reranker clip at 2,000 as always */

bool memory_index_text_enabled(void);
/* The index text for a unit's original text: the file's resolution, else the
 * text itself; NULL when the experiment is off. The pointer is owned by the
 * table or is `text`: valid as long as either is. */
const char *memory_index_text_for(const char *text);
/* The column the lexical queries match: "u.index_text" or "u.text". */
const char *memory_index_text_match_column(void);
/* Strict UTF-8, 1..MEMORY_INDEX_TEXT_BYTES bytes: what the file and the vector
 * index accept as an index text. */
bool memory_index_text_valid(const char *text, size_t *bytes);

/* Tests: load a file explicitly, or drop the table and disable. */
bool memory_index_text_load(const char *path, char *err, size_t errlen);
void memory_index_text_disable(void);

#endif
