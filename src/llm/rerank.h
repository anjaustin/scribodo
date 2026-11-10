/* Cross-encoder relevance scores from the local Qwen3-Reranker through
 * llama-server: P(yes) against P(no) at the first predicted token under the
 * model card's prompt. Measured against the PyTorch reference in
 * journal/answer-cycle2_rerank.md (build/rr1): first place agrees on 8 of 8
 * questions, decisions agree on 89 of 89 pairs, maximum score difference
 * 0.026 (BF16 against float32). A NULL server gives a deterministic
 * content-word overlap fixture for tests and the fake backend; it is never
 * a quality claim. */
#ifndef SCRIBODO_RERANK_H
#define SCRIBODO_RERANK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "llm/server.h"

typedef struct RerankClient RerankClient;

RerankClient *rerank_client_create(LlmServer *server);
void rerank_client_destroy(RerankClient *c);
bool rerank_client_fixture(const RerankClient *c);
/* The server is READY and has answered the one-time token lookup. */
bool rerank_client_ready(RerankClient *c, atomic_bool *cancel, double deadline_ms);
/* One (question, text) pair. `text` is clipped to 2,000 bytes on a character
 * boundary, as production embeddings are. Fails closed on any transport,
 * decoding or missing-token condition; `err` says why. */
bool rerank_score(RerankClient *c, const char *question, const char *text, atomic_bool *cancel,
                  double deadline_ms, double *p_yes, char *err, size_t errlen);

/* ---- exposed for tests ----------------------------------------------- */
char *rerank_prompt(const char *question, const char *text);  /* owned; neutralized inputs */
char *rerank_request(const char *prompt);                     /* owned JSON for /completion */
bool rerank_parse(const char *body, int yes_id, int no_id, double *p_yes);
double rerank_fixture_score(const char *question, const char *text);

#endif
