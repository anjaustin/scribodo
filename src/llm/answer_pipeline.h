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
/* What the composer window shows in place of an earlier refusal (a withheld notice or a gated abstention) when the
 * current turn is a question; measured in journal/answer-cycle4_window.md. On other turns refusals stay as recorded. */
#define ANSWER_WINDOW_MARKER "(No answer was given.)"
#define ANSWER_SPANS 2u /* pass 3: the top two units each contribute one byte-verified span */
#define ANSWER_SCORED_MAX (ANSWER_CANDIDATES * ANSWER_SPANS) /* cycle 3: one candidate pool per question part */

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
    size_t index_text_units; /* diagnostic: hydrated units reranked on their index text (app/memory_index_text.h) */
    /* Cycle 3: the request's question parts, each with its context sentence; one slot per part, assigned as a
     * maximum-score matching (journal/answer-cycle3_relation.md, arm 4). top_part/second_part index parts[]. */
    size_t nparts;
    char *parts[ANSWER_SPANS]; /* owned */
    size_t top_part, second_part;
    char scored_ids[ANSWER_SCORED_MAX][13]; /* diagnostic: every scored (part, unit) pair, in scoring order */
    double scored_scores[ANSWER_SCORED_MAX];
    unsigned char scored_part[ANSWER_SCORED_MAX];
} AnswerPipelineResult;

bool answer_pipeline_run(Db *db, SemanticClient *vectors, RerankClient *rerank, LlmServer *chat_server,
                         const MemoryScope *scope, const MemoryDocumentSet *documents, const char *query,
                         int64_t before, atomic_bool *cancel, double deadline_ms, AnswerPipelineResult *out);
void answer_pipeline_result_free(AnswerPipelineResult *r);

/* What the request carries: the block goes before the author's newest words. The reply's receipt
 * (memory_sources provenance) is llm/answer_receipt.h. */
char *answer_verified_block(const AnswerPipelineResult *r);
const char *answer_source_name(const MemoryUnit *u); /* the document name, or "an earlier reply" / "an earlier note" */
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
/* The reply shown instead of a withheld answer: the abstention and the verified passages. `reason` is the clause
 * after "An answer was withheld because"; the two-argument form is the gate's. */
char *answer_withheld_reply(const char *const *names, const char *const *spans, size_t nspans);
char *answer_withheld_reply_reason(const char *const *names, const char *const *spans, size_t nspans, const char *reason);

/* ---- quotation integrity (cycle 3, the coreference live pass) ------------ */
/* Every quotation in the composed reply must be text the author can verify: a substring of a verified span, of
 * one of the author's own turns, or of the question, under the gate's normalization (case, hyphens, emphasis
 * marks and quote styles folded, surrounding punctuation dropped). A quotation is the text between a pair of
 * double quotes (straight or curly) or a markdown blockquote line; an ellipsis splits it into pieces checked
 * separately; a piece shorter than ANSWER_MIN_SPAN is a mention, not a quotation, and is not checked; an
 * unterminated quote mark is a slip, not a quotation. Returns the number of pieces checked; `*misquoted` gets the
 * first failing piece (owned) or NULL. The live treatment of journal/answer-cycle3_coref.md showed a reply that
 * quoted a verified span with "daughters" rewritten as "sons" and asserted the swap; this is that check. */
size_t answer_quotations(const char *reply, const char *const *spans, size_t nspans, const char *const *author_turns,
                         size_t nturns, const char *question, char **misquoted);

/* ---- relation integrity (cycle 4, the relation item) --------------------- */
/* A relation the composer states in its own words must be one the sources state: kinship and possession phrases
 * in the reply's prose (quotations and blockquote lines excluded), "<kin> of <Name>", "<Name>'s <kin>", "<his|her|
 * their|Name> <kin> is <Name>", "<Name> is the <kin> of <Name>", with kin from a closed list (father, mother, son,
 * daughter, sister(s), brother(s), wife, husband, friend, estate, fortune, inheritance, property) and Name a
 * capitalized run (honorific allowed; sentence starters and labels are not names). A phrase is supported when its
 * normalized text is a substring of a verified span, an author turn or the question ("X's Y" also by "Y of X"); a
 * sentence carrying a negation (not, never, neither, nor, wrong, instead, isn't, aren't, wasn't) is skipped,
 * because a correct correction states the wrong relation to deny it. Returns the number of phrases found;
 * `*unsupported` gets the first unsupported phrase (owned) or NULL. The arm that measured this on 60 recorded
 * replies (7 flagged, every one with a relation the sources do not state, no correct answer among them) is
 * tools/answer_relation_arm.py; tests/fixtures/answer_relation/replies.json pins the two to each other
 * (journal/answer-cycle4_relation.md). */
size_t answer_relations(const char *reply, const char *const *spans, size_t nspans, const char *const *author_turns,
                        size_t nturns, const char *question, char **unsupported);

/* ---- question parts and slot assignment (cycle 3) ----------------------- */
/* The request's question sentences, each conjunction-joined wh-part on its own ("..., and why ..." starts a new
 * part), each ending in "?", each prefixed with the request's non-question sentences (the referent a bare part
 * loses). A request with no question sentence is one part, unchanged. At most ANSWER_SPANS parts; owned strings.
 * Software only; the same rule as tools/answer_question_parts.py. */
size_t answer_question_parts(const char *question, char *parts[ANSWER_SPANS]);
/* Slots from per-part scores over n units (a score below 0 means not in that part's pool or not scored). Two parts:
 * the distinct pair maximizing the score sum; one part, or one empty pool: the two highest of the pool that has
 * units. slot[k] is SIZE_MAX when empty; slot_part[k] names the part the slot answers. */
bool answer_assign_slots(const double *s0, const double *s1, size_t n, size_t slot[ANSWER_SPANS], size_t slot_part[ANSWER_SPANS]);

/* ---- pure helpers, exposed for tests ----------------------------------- */
/* Folds typographic quotes and dashes, drops Markdown emphasis, collapses
 * whitespace, lowercases ASCII: the citation classifier's normalization. */
char *answer_normalize(const char *text);
/* Exact normalized substring, else the longest sentence-boundary prefix that
 * is; at least ANSWER_MIN_SPAN characters. `status` names the outcome.
 * `*verified` is the unit's own bytes at the match (widened over quotation
 * marks and emphasis markers at its edges), never the model's rendering. */
bool answer_verify_span(const char *span, const char *unit_text, char **verified, const char **status);
char *answer_extraction_request(const char *question, const char *unit_text); /* owned JSON */
/* `parsed` is false when the reply was not the schema's JSON (for example cut
 * by the token cap); then *span is NULL. */
bool answer_extraction_parse(const char *response_body, char **span, bool *parsed);

#endif
