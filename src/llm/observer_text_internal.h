#ifndef SCRIBODO_OBSERVER_TEXT_INTERNAL_H
#define SCRIBODO_OBSERVER_TEXT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

/* Pure lexical helpers shared with the observer's deterministic backend and
 * chat-finding rules. They own no observer or worker state. */
bool obs_word_byte(unsigned char c);
bool obs_upper_start(const unsigned char *word);
bool obs_stopword(const char *word, size_t len);

#endif
