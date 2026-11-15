#include "app/local_models.h"

#include <stdio.h>
#include <unistd.h>

bool local_models_reranker_present(const LlmServerConfig *rerank)
{
    if (access(rerank->model_path, R_OK) == 0) return true;
    fprintf(stderr, "scribodo: no reranker model at %s; answers are not verified (see docs/MODEL-002-qwen3-reranker.md)\n",
            rerank->model_path);
    return false;
}

int local_models_reap_orphans(const LlmServerConfig *chat, const LlmServerConfig *embed, const LlmServerConfig *rerank)
{
    int reaped = 0;
    if (llm_server_reap_orphan(chat)) {
        fprintf(stderr, "scribodo: reaped the chat server of an earlier window\n");
        reaped++;
    }
    if (llm_server_reap_orphan(embed)) {
        fprintf(stderr, "scribodo: reaped the embedding server of an earlier window\n");
        reaped++;
    }
    if (rerank && llm_server_reap_orphan(rerank)) {
        fprintf(stderr, "scribodo: reaped the reranker of an earlier window\n");
        reaped++;
    }
    return reaped;
}
