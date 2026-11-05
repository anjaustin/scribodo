/* Noticing names in chat (user testing 2026-11-04): when the author says
 * what they're called, or what to call the assistant, offer to save it as
 * a setting (src/app/profile.h). Settings are the source of truth; this
 * only suggests, and the author confirms.
 *
 * The local model extracts candidates with structured output (a JSON
 * schema). A candidate is only kept if it appears word for word in the
 * author's own message, so a name is never invented. Without a model (UI
 * tests) a few fixed phrasings are recognised instead. */
#ifndef SCRIBODO_NAMES_H
#define SCRIBODO_NAMES_H

#include <stdbool.h>
#include <stddef.h>

#include "llm/server.h"

typedef struct {
    char author[64];    /* "" = none found */
    char assistant[64]; /* "" = none found */
} NameGuess;

/* Worth asking the model about at all (a cheap test on the message). */
bool names_worth_checking(const char *author_msg);

/* Blocking. `assistant_prev` is the assistant's message before the
 * author's (it gives "How about Maz?" its meaning); may be NULL. `server`
 * NULL: the fixed phrasings. */
bool names_detect(LlmServer *server, const char *assistant_prev, const char *author_msg, NameGuess *out);

/* Exposed for tests: is `name` in `text` as whole words? */
bool names_in_text(const char *name, const char *text);

#endif
