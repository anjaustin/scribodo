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

/* Split model output into visible text and reasoning. A leading
 * <think>...</think> block (unterminated while streaming) is reasoning; an
 * orphan </think> marks everything before it as reasoning; any other <think>
 * is left visible. Either output may be NULL; both are malloc'd. */
void llm_split_thinking(const char *s, char **visible, char **reasoning);

/* Clean a model reply meant to be a drop-in replacement for `passage`:
 * strip surrounding whitespace, a wrapping ``` fence, a one-line preamble
 * ("Here is the rewritten passage:") and wrapping quotes the passage itself
 * did not have, then restore the passage's exact leading/trailing
 * whitespace. Returns malloc'd text; "" if the reply had no content. */
char *llm_clean_edit(const char *reply, const char *passage);

#endif
