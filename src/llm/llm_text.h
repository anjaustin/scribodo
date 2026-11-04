/* Text rules for everything sent to and received from the model.
 *
 * Neutralisation: LFM3's tokenizer turns literal control sequences such as
 * "<|im_start|>" into real control tokens even inside message content, so a
 * manuscript or pasted text could forge conversation turns (MODEL-001,
 * "Prompt injection through special tokens"). We break every such sequence by
 * inserting U+200C ZERO WIDTH NON-JOINER after the '<'. The text stays
 * visually identical; the tokenizer no longer matches the special token.
 *
 * Reasoning: the model may emit <think>...</think> blocks even with thinking
 * disabled. They are separated from the visible reply, and never sent back in
 * history (llama-server would otherwise keep them; MODEL-001).
 */
#ifndef SCRIBODO_LLM_TEXT_H
#define SCRIBODO_LLM_TEXT_H

#include <stdbool.h>
#include <stddef.h>

/* Returns a malloc'd copy of `s` with every "<|", "<think" and "</think"
 * broken by U+200C after the '<'. Never returns NULL for non-NULL input
 * unless allocation fails. */
char *llm_neutralize(const char *s);

/* True if `s` contains a sequence llm_neutralize() would break. */
bool llm_needs_neutralize(const char *s);

/* Split model output into visible text and reasoning. Removes every
 * <think>...</think> block (an unterminated block runs to the end of the
 * text, as while streaming). Either output may be NULL if not wanted; both
 * are malloc'd. */
void llm_split_thinking(const char *s, char **visible, char **reasoning);

#endif
