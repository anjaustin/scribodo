/* The answer receipt and trace contract.
 *
 * A reply produced by the verified pipeline (SCRIBODO_ANSWER_PIPELINE=verified)
 * carries a receipt in ChatMessage.memory_sources: the stored provenance of
 * what the composer was shown and what the gate decided. It persists in
 * messages.memory_sources, is bounded by the store at CHAT_MEMORY_SOURCES_BYTES
 * (16 KiB; the row is refused above it), is shown to the author in the Sources
 * panel, and is read by chat_receipt_refusal() (llm/chat_history.c) when the
 * composer window is built and by tools/answer_pipeline_review.py when a run
 * is reviewed. This header is the one definition of the format. The C test
 * (tests/test_answer_pipeline.c, test_receipt_contract) writes every line into
 * tests/fixtures/answer_receipt/ and the Python test
 * (tests/test_answer_receipt_contract.py) parses those same files with the
 * review tool's expressions; change the format here, regenerate the fixtures
 * (SCRIBODO_WRITE_FIXTURES=1 ./build/test_answer_pipeline), and both tests
 * say what else must follow.
 *
 * Receipt lines, in this order, each ending in " ---\n". Fields are separated
 * by " | "; a field is "name=value" and no value contains "\n" or " | ".
 *
 *   --- VERIFIED SOURCE | unit=<id> | document=<name> | score=<0.0000> | status=<status> | part=<n> | candidates=<n> scored=<n> ---
 *   <verified span of the first unit, as quoted to the composer>
 *   --- VERIFIED SOURCE | none | status=<status> | candidates=<n> scored=<n> ---
 *       (instead of the two lines above when no unit was chosen; no span line)
 *   --- VERIFIED SOURCE 2 | unit=<id> | document=<name> | score=<0.0000> | status=<status> | part=<n> ---
 *   <verified span of the second unit>
 *       (only when a second unit was ranked; its span line may be empty)
 *   --- ANSWER GATE | verdict=<verdict> | withheld=<0|1> | answer=<claim> ---
 *   --- ANSWER QUOTES | checked=<n> | misquoted=<text> ---
 *       (both appended by the gate after the reply finished; absent when the
 *       gate did not run, which is any turn that is not a question)
 *
 * <id> is the memory unit id (64 hex digits in the product; tests use short
 * ids). <name> is the document name as stored, or "an earlier reply" / "an
 * earlier note" for chat units. <status> is one of the AnswerPipelineResult
 * status words (answer_pipeline.h). part= is 1-based. <verdict> is one of
 * answer_gate()'s words: pass | pass_no_anchors | no_answer | echoes_question |
 * not_in_span, or "unavailable" when the claim request failed. withheld=1 when
 * the shown reply was replaced by the abstention, for either the verdict or a
 * misquotation. answer= is the claim the model gave for its own reply;
 * misquoted= is the first quoted phrase the verified text did not contain, or
 * empty. These two are free text: newlines become spaces, the field is cut at
 * ANSWER_RECEIPT_FIELD_MAX bytes on a UTF-8 boundary, and it runs to the
 * line's final " ---", so a parser takes everything up to the end of line.
 * A refusal, for the composer window, is withheld=1 or verdict=no_answer.
 *
 * Bound. answer_receipt() never exceeds ANSWER_RECEIPT_PIPELINE_MAX bytes: the
 * header lines are always complete and the span lines are cut, on a UTF-8
 * boundary, to fit. answer_receipt_append_gate() keeps the whole receipt at
 * or under CHAT_MEMORY_SOURCES_BYTES by cutting answer= first and misquoted=
 * second, down to empty; it fails, leaving the receipt unchanged, only when
 * the two lines do not fit even then, which the pipeline bound prevents.
 *
 * Trace lines (stderr, only under SCRIBODO_MEMORY_TRACE=1; space-separated
 * name=value fields, one line each, parsed by tools/answer_pipeline_review.py):
 *
 *   ANSWER_PIPELINE candidates=<n> hydrated=<n> scored=<n> failures=<n> top=<id|-> score=<0.0000> status=<status> second=<id|-> second_score=<0.0000> second_status=<status|-> parts=<n> top_part=<n> second_part=<n|0> rerank_ms=<0.0> extract_ms=<0.0> request_bytes=<n> history_first=<n>
 *   ANSWER_PIPELINE_SCORES part=<n> <id12>:<0.0000> ...      (one line per question part; ids are 12-character prefixes)
 *   ANSWER_INDEX_TEXT enabled=1 units=<n> hydrated=<n>       (only with SCRIBODO_INDEX_TEXT_FILE set)
 *   ANSWER_GATE verdict=<verdict> withheld=<0|1> parsed=<0|1> spans=<n> author_turns=<n> ms=<0.0> answer_bytes=<n> quotations=<n> misquoted=<0|1>
 *
 * MEMORY_PREPARE (the planner's timing line) is written by chat.c and is not
 * part of this contract; no tool parses it. Records: journal/answer-cycle2_*,
 * answer-cycle3_relation.md (parts), answer-cycle4_window.md (the refusal rule),
 * answer-cycle4_receipt.md (this contract). */
#ifndef SCRIBODO_ANSWER_RECEIPT_H
#define SCRIBODO_ANSWER_RECEIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "llm/answer_pipeline.h"
#include "llm/chat.h"

#define ANSWER_RECEIPT_FIELD_MAX 1024u /* answer= and misquoted= are cut here */
/* Room the gate's two lines may need at most, reserved out of the store bound by the pipeline receipt. */
#define ANSWER_RECEIPT_GATE_RESERVE (2u * ANSWER_RECEIPT_FIELD_MAX + 256u)
#define ANSWER_RECEIPT_PIPELINE_MAX (CHAT_MEMORY_SOURCES_BYTES - ANSWER_RECEIPT_GATE_RESERVE)

/* The pipeline's receipt for a reply: owned, at most ANSWER_RECEIPT_PIPELINE_MAX bytes, NULL only on no memory. */
char *answer_receipt(const AnswerPipelineResult *r);
/* Appends the gate's two lines to *memory_sources (which may be NULL) in place, keeping the whole at or under
 * CHAT_MEMORY_SOURCES_BYTES. False, with *memory_sources unchanged, on no memory or when even the empty-field
 * lines do not fit. */
bool answer_receipt_append_gate(char **memory_sources, const char *verdict, bool withheld, const char *answer,
                                size_t quotations, const char *misquoted);
/* The trace lines, written only by the callers that checked the trace flag. */
void answer_trace_pipeline(FILE *out, const AnswerPipelineResult *r, size_t request_bytes, size_t history_first,
                           bool index_text_enabled);
void answer_trace_gate(FILE *out, const char *verdict, bool withheld, bool parsed, size_t spans, size_t author_turns,
                       double ms, size_t answer_bytes, size_t quotations, bool misquoted);

#endif
