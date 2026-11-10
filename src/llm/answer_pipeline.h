/* Verified-answer pipeline (experiment, SCRIBODO_ANSWER_PIPELINE=verified):
 * a wider first stage (lexical and semantic candidates, 32 each), one unit
 * chosen by the local cross-encoder, one extraction call that copies the
 * stating sentence, a byte check of that span against the unit, and a
 * request that carries only the verified span before the author's words.
 * Nothing unverified reaches the model as evidence; with no verified span the
 * request says so. Decided from the measurements in
 * journal/answer-cycle2_unit_extraction.md, _ranked_span.md and _rerank.md. */
#ifndef SCRIBODO_ANSWER_PIPELINE_H
#define SCRIBODO_ANSWER_PIPELINE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app/memory_documents.h"
#include "app/memory_store.h"
#include "llm/rerank.h"
#include "llm/semantic.h"
#include "llm/server.h"

#define ANSWER_CANDIDATES 64u
#define ANSWER_MIN_SPAN 12u
#define ANSWER_PIPELINE_BUDGET_MS 90000.0
#define ANSWER_HISTORY_MESSAGES 6u /* the composer window: newest three pairs plus the verified block */
#define ANSWER_SPANS 2u /* pass 3: the top two units each contribute one byte-verified span */

typedef struct {
    bool attempted;
    size_t candidates, hydrated, scored, score_failures;
    bool has_top;
    MemoryUnit top;      /* owned when has_top: the first chosen unit */
    double top_score;
    char *raw_span;      /* owned model output for the first unit, or NULL */
    char *span;          /* owned verified span from the first unit, or NULL */
    char status[48];     /* verified | verified_prefix | none | rejected_not_in_unit | too_short | unparsed |
                            no_candidates | rerank_unavailable | extraction_failed | canceled */
    bool has_second;     /* the second-ranked unit, same fields (pass 3) */
    MemoryUnit second;
    double second_score;
    char *second_raw_span, *second_span;
    char second_status[48];
    double rerank_ms, extract_ms;
    char scored_ids[ANSWER_CANDIDATES][13]; /* diagnostic: every scored unit (id prefix) and its score, in scoring order */
    double scored_scores[ANSWER_CANDIDATES];
} AnswerPipelineResult;

bool answer_pipeline_run(Db *db, SemanticClient *vectors, RerankClient *rerank, LlmServer *chat_server,
                         const MemoryScope *scope, const MemoryDocumentSet *documents, const char *query,
                         int64_t before, atomic_bool *cancel, double deadline_ms, AnswerPipelineResult *out);
void answer_pipeline_result_free(AnswerPipelineResult *r);

/* What the request carries. The block goes before the author's newest words;
 * the receipt is the reply's memory_sources provenance. */
char *answer_verified_block(const AnswerPipelineResult *r);
char *answer_receipt(const AnswerPipelineResult *r);
extern const char *const answer_composer_instruction;

/* ---- the answer-in-span gate (pass 4) ---------------------------------- */
/* Applies to requests that are questions: the stand-in for a request classifier. */
bool answer_gate_applies(const char *question);
char *answer_claim_request(const char *question, const char *reply);      /* owned JSON: what answer does the reply give */
bool answer_claim_parse(const char *response_body, char **answer, bool *parsed);
/* "pass" | "pass_no_anchors" | "no_answer" | "echoes_question" | "not_in_span". The claim's anchor tokens (numbers in
 * digit or word form, capitalized words) must each occur in a verified span or one of the author's own turns; anchors
 * all inside the question are an echo; an abstention in the claim's words is no answer (pass 5). */
const char *answer_gate(const char *answer, const char *question, const char *const *spans, size_t nspans,
                        const char *const *author_turns, size_t nturns);
/* The reply shown instead of a withheld answer: the abstention and the verified passages. */
char *answer_withheld_reply(const char *const *names, const char *const *spans, size_t nspans);

/* ---- pure helpers, exposed for tests ----------------------------------- */
/* Folds typographic quotes and dashes, drops Markdown emphasis, collapses
 * whitespace, lowercases ASCII: the citation classifier's normalization. */
char *answer_normalize(const char *text);
/* Exact normalized substring, else the longest sentence-boundary prefix that
 * is; at least ANSWER_MIN_SPAN characters. `status` names the outcome. */
bool answer_verify_span(const char *span, const char *unit_text, char **verified, const char **status);
char *answer_extraction_request(const char *question, const char *unit_text); /* owned JSON */
/* `parsed` is false when the reply was not the schema's JSON (for example cut
 * by the token cap); then *span is NULL. */
bool answer_extraction_parse(const char *response_body, char **span, bool *parsed);

#endif
