/* Parallel observer (SOW §4.3): reads what the author has *stored* (saved
 * documents and saved chat messages) and records findings: entities, events,
 * relationships, open questions, and possible conflicts with committed
 * canon. It never speaks into the chat and never changes documents, notes
 * or canon; findings appear only in the observer sidebar.
 *
 * Work is derived from stored state, so nothing is lost across restarts:
 * <project>/.scribodo/observer/state.json remembers which document
 * paragraphs (by hash) and how many messages of each chat have been read.
 * After a save or a finished reply the app calls observer_poke(); at
 * project open everything not yet read is picked up.
 *
 * Scheduling: the observer shares the model with chat, and its prompts
 * stall a streaming reply for seconds (docs/MODEL-001). It runs only while
 * chat has been idle for a moment, and an in-flight request is cancelled
 * (and retried later) as soon as chat becomes active.
 *
 * Every finding's evidence must be found in the source text, or the finding
 * is dropped: each one links to an exact range (a revision snapshot of the
 * document, or a chat message).
 */
#ifndef SCRIBODO_OBSERVER_H
#define SCRIBODO_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/records.h"
#include "llm/server.h"

typedef struct Observer Observer;

/* `server` NULL: the built-in deterministic fake (UI tests). */
Observer *observer_create(LlmServer *server);
void observer_destroy(Observer *o);

/* Switch project ("" for none): cancels the current job and starts over
 * from that project's stored state. */
void observer_set_project(Observer *o, const char *project_dir);
/* A document was renamed: what was read of it stays read. */
void observer_rename_doc(Observer *o, const char *project_dir, const char *from, const char *to);
/* Drop the project's semantic index and rebuild it from the store (on the
 * worker, between jobs; the status line reports progress). */
void observer_request_rebuild(Observer *o);
/* Something was stored (a save, a finished reply): look for new work. */
void observer_poke(Observer *o);
/* Call every frame. While true, observer requests are cancelled and none
 * start; work resumes once chat has been idle for a short while. */
void observer_set_chat_active(Observer *o, bool active);
/* Pause/resume (e.g. a setting); paused observers finish nothing new. */
void observer_set_enabled(Observer *o, bool enabled);

/* Short status for the sidebar ("Up to date", "Reading ch1.md…", ...). */
void observer_status(Observer *o, char *out, size_t outlen);
/* Bumps whenever findings were written (the app re-lists them). */
uint64_t observer_version(Observer *o);
/* Nothing running or waiting to run (tests). */
bool observer_idle(Observer *o);
/* Jobs started, including failed ones (tests). */
unsigned observer_attempts(Observer *o);
/* Jobs finished since creation (tests). */
unsigned observer_jobs_done(Observer *o);

/* Change a finding's status ("saved", "committed", "dismissed"), as a new
 * version. Serialised with the observer's own writes. */
bool observer_set_finding_status(Observer *o, const char *project_dir, const char *id, const char *status);

/* ---- internals, exposed for unit tests --------------------------------------------- */

/* Find `evidence` in text[0, n): exact, then ignoring differences in quote
 * style, whitespace and emphasis markers (* _), then by its longest prefix
 * of at least 4 words. Returns the byte range of the matching source text. */
bool obs_locate(const char *text, size_t n, const char *evidence, size_t *start, size_t *len);

/* Relevance score between two texts: shared names count 2, other shared
 * content words 1. Used to choose which canon entries to check. */
int obs_overlap(const char *a, const char *b);

/* Paragraphs of a document: byte ranges split on blank lines. */
size_t obs_paragraphs(const char *text, size_t *starts, size_t *lens, size_t max);

/* The fake backend's answers (deterministic; same JSON as the model). */
char *obs_fake_extract(const char *chunk);
char *obs_fake_judge(const char *canon, const char *statement);

/* Exposed for tests: a chat finding that describes the conversation
 * itself rather than the story world (dropped). */
bool obs_about_the_conversation(const char *subject, const char *desc);

/* Who is who (stored names): the author and the assistant are never
 * findings in chats. Changing them re-checks the project's chat findings. */
void observer_set_names(Observer *o, const char *author, const char *assistant);
/* Exposed for tests: whether a chat finding is about the story world (not
 * the conversation or the two people in it), and the re-check of a
 * project's chat findings under the current rules. */
bool obs_chat_finding_allowed(const FindingInfo *f, const char *desc, const char *author, const char *assistant);
size_t obs_refilter(const char *project, const char *author, const char *assistant);
#endif
