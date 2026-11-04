/* Supervisor for the local llama-server process (see docs/MODEL-001).
 *
 * On start (in a background thread):
 *   1. Verify the model file: pinned SHA-256, cached by (path, size, mtime,
 *      inode) so the 3.5 GB file is hashed only once.
 *   2. Kill an orphaned server from a previous crash (pid file), remove a
 *      stale socket, write a fresh random API key (0600) in a 0700 run dir.
 *   3. Spawn llama-server with the validated BF16 flags on a Unix socket
 *      (no TCP port), logging to the app's log directory.
 *   4. Poll /health until ready.
 * While running, llm_server_tick() reaps a crashed server and restarts it
 * with backoff. llm_server_stop() terminates it and cleans up.
 */
#ifndef SCRIBODO_LLM_SERVER_H
#define SCRIBODO_LLM_SERVER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LLM_STOPPED = 0,
    LLM_VERIFYING,  /* hashing the model file */
    LLM_STARTING,   /* process launched, loading the model */
    LLM_READY,
    LLM_RESTARTING, /* crashed; waiting to restart */
    LLM_FAILED,     /* gave up; see message */
} LlmState;

typedef struct {
    char server_bin[1024];  /* llama-server executable */
    char model_path[1024];  /* BF16 GGUF */
    char sha256[65];        /* expected hex digest; empty = skip (tests only) */
    char run_dir[512];      /* 0700; holds socket, key, pid file */
    char log_path[1024];
    char cache_path[1024];  /* verified-hash cache */
    int ctx_total;          /* -c (split evenly across slots) */
    int slots;              /* -np */
    int batch;              /* -b and -ub */
    bool embedding;         /* an embedding server (Qwen3-Embedding), not the chat model */
} LlmServerConfig;

typedef struct LlmServer LlmServer;

/* Fill `cfg` with the default locations and validated settings. Paths that
 * cannot be resolved are left empty; llm_server_start() reports why. */
void llm_server_default_config(LlmServerConfig *cfg, const char *app_dir);
/* The embedding server: Qwen3-Embedding-0.6B (F16, Apache-2.0, 1,024
 * dimensions, pinned hash), its own socket and run folder, one slot. */
void llm_server_embed_config(LlmServerConfig *cfg, const char *app_dir);

LlmServer *llm_server_create(const LlmServerConfig *cfg);
void llm_server_destroy(LlmServer *s); /* stops it first */

void llm_server_start(LlmServer *s);
void llm_server_stop(LlmServer *s);

/* Call regularly (e.g. each frame): handles crash detection and restarts. */
void llm_server_tick(LlmServer *s);

/* Current state; `msg` (optional) receives a human-readable status line. */
LlmState llm_server_state(LlmServer *s, char *msg, size_t msglen);

/* Context available to one request: -c is split evenly across slots. */
int llm_server_slot_ctx(LlmServer *s);

/* Valid while the server is READY. */
const char *llm_server_socket(LlmServer *s);
const char *llm_server_key(LlmServer *s);

/* Test hooks. */
/* Mark READY on an externally provided socket (a stub server) without
 * launching anything. */
void llm_server_test_attach(LlmServer *s, const char *socket_path, const char *key);
int llm_server_pid(LlmServer *s);
int llm_server_restarts(LlmServer *s);

#endif
