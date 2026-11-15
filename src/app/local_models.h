/* Startup checks for the pinned local model servers, kept out of main() (AUDIT-004 F3). */
#ifndef SCRIBODO_LOCAL_MODELS_H
#define SCRIBODO_LOCAL_MODELS_H

#include <stdbool.h>

#include "llm/server.h"

/* The verified-answer pipeline needs the pinned reranker file. False when this Mac has none, and then says so on
 * stderr: the plain composer answers, and the toolbar's memory line says why. */
bool local_models_reranker_present(const LlmServerConfig *rerank);

/* Servers an earlier window left behind hold their memory until they are reaped; reap before the memory fit reads
 * what is free, or a relaunch after a crash runs the smallest context (GAP-003 restart). `rerank` is NULL when the
 * reranker is not used. Each reap is reported on stderr; returns how many were reaped. */
int local_models_reap_orphans(const LlmServerConfig *chat, const LlmServerConfig *embed, const LlmServerConfig *rerank);

#endif
