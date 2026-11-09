/* A creative-chat conversation with the local model.
 *
 * chat_send() appends the author's message and an empty assistant message,
 * then streams the reply into it on a worker thread. The UI reads messages
 * under chat_lock()/chat_unlock() and redraws when chat_version() changes.
 *
 * Every request applies the rules from docs/MODEL-001:
 *   - all user and document text is neutralised (special-token injection);
 *   - earlier reasoning is never sent back, and stray <think> blocks are
 *     removed from what the author sees;
 *   - thinking is disabled (enable_thinking=false);
 *   - the conversation is trimmed to the slot's context budget, keeping the
 *     system prompt and document (stable prefix) first.
 */
#ifndef SCRIBODO_CHAT_H
#define SCRIBODO_CHAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llm/server.h"
#include "llm/remote.h"

typedef enum { CHAT_USER = 0, CHAT_ASSISTANT = 1 } ChatRole;

typedef struct {
    ChatRole role;
    char *content;   /* visible text (UTF-8) */
    bool error;      /* assistant message that failed */
    bool streaming;  /* still being generated */
    bool truncated;  /* reply stopped at the length limit */
    bool stopped;    /* reply stopped by the author part-way (incomplete) */
    bool is_edit;    /* user: an edit request; assistant: a proposed replacement */
    char *docs;      /* user: names of the documents attached, '\n'-separated ("" = none), or NULL */
    char *via;       /* sent to / answered by a remote model ("Frontier · api.example.com"); NULL = this Mac */
    double ts;       /* when it was stored (loaded chats), else 0 */
    bool sequence_known; /* persisted DB sequence, never a window offset */
    int64_t sequence;
    bool excerpt;    /* stored body is only a display range; not model history */
    bool excerpt_more;
    size_t excerpt_start; /* codepoint offset in the persisted original */
    char *memory_sources; /* optional owned as-of local retrieval provenance, never model history */
} ChatMessage;

#define CHAT_MEMORY_SOURCES_BYTES (16u * 1024u)

struct StoreManager;
struct SemanticClient;
struct GenerativeGate;
struct MemoryDocumentCache;

typedef struct {
    const char *title;
    const char *text;
    bool unsaved; /* dirty open buffer: foreground input only, never saved memory */
    bool truncated; /* bounded explicit snapshot; preserve a visible cutoff notice */
} ChatDoc;

/* Reference material for a request. `docs` (several attached documents)
 * takes precedence; doc_title/doc_text is a single document (edit requests:
 * the document being edited). Each is treated as material, not instructions. */
typedef struct {
    const char *doc_title; /* NULL = no document attached */
    const char *doc_text;
    const ChatDoc *docs;
    size_t ndocs;
} ChatContext;

typedef struct ChatSession ChatSession;

/* Defence in depth behind the transport's wire limit. */
#define CHAT_MAX_RESPONSE_BYTES REMOTE_MAX_RESPONSE_BYTES

/* Called on the UI thread from chat_poll() when a message is final. */
typedef void (*ChatMessageDone)(ChatSession *s, size_t index, void *ctx);

/* `server` may be NULL to use the built-in fake model (UI tests). */
ChatSession *chat_create(LlmServer *server);
void chat_destroy(ChatSession *s);

/* Starts streaming a reply. Fails (returns false with a reason) if a reply
 * is already streaming or the model isn't ready. The request (including
 * exact token budgeting via the server's tokenizer) is built on the worker
 * thread; `ctx` is copied. */
bool chat_send(ChatSession *s, const char *text, const ChatContext *ctx, char *err, size_t errlen);
/* Request a rewrite of `passage` (text the author selected) following
 * `instruction`. The reply is a proposed replacement, flagged is_edit; it is
 * never applied by the chat itself (see the app's review flow). */
bool chat_send_edit(ChatSession *s, const char *instruction, const char *passage, const ChatContext *ctx,
                    char *err, size_t errlen);
void chat_cancel(ChatSession *s);
bool chat_busy(ChatSession *s);
/* Experimental, local-only retrieval. Idle UI command; borrowed dependencies
 * outlive the session or must be cleared after joining its worker. */
bool chat_memory_configure(ChatSession *s, struct StoreManager *store, struct SemanticClient *vectors,
                            const char *project_dir, const char *conversation, bool enabled);
/* UI mutation fence, safe while preparation is running. Changes affect future
 * admissions, not a promise to retract requests already admitted. */
void chat_memory_attachments_changed(ChatSession *s);
uint64_t chat_memory_attachment_version(ChatSession *s);
bool chat_set_generative_gate(ChatSession *s, struct GenerativeGate *gate);
bool chat_memory_set_document_cache(ChatSession *s, struct MemoryDocumentCache *cache);
bool chat_memory_set_briefs_enabled(ChatSession *s, bool enabled);
/* Experimental canon-aware chat (MEMORY-014 P1): supply the author's matching
 * committed canon as an exact, separate reference class. Idle-only; never
 * grants memory consent or scope; never clips the author's request. */
bool chat_set_canon_reference(ChatSession *s, bool enabled);
typedef enum { CHAT_MEMORY_LEGACY, CHAT_MEMORY_COMPACT, CHAT_MEMORY_EPISODE } ChatMemoryPresentation;
/* Idle-only experimental representation; never grants memory consent/scope. */
bool chat_memory_set_presentation(ChatSession *s, ChatMemoryPresentation presentation);
/* Owned deterministic representation of consecutive historical author turns.
 * Preserves content/order and the separate current request; NULL leaves caller
 * free to retain the original request. Caller must count the complete result. */
char *chat_pack_history_request(const char *request);
/* Owned bounded as-of sources for the last prepared local reply; NULL if none. */
char *chat_memory_sources(ChatSession *s);

/* Load an existing conversation (replaces the current one). Not while busy.
 * Allocation/validation failure leaves the current conversation intact. */
bool chat_load_messages(ChatSession *s, const ChatMessage *msgs, size_t n);
/* UI-thread commands for a DB-backed bounded display window. Caller owns msgs.
 * Idle, saved history only; not concurrent with another UI command/send. */
bool chat_load_window(ChatSession *s, const ChatMessage *msgs, size_t n, bool older, bool newer);
typedef struct {
    bool active, older, newer, unsaved;
    int64_t first, last;
} ChatWindowInfo;
ChatWindowInfo chat_window_info(ChatSession *s); /* lock held */
long chat_find_sequence(ChatSession *s, int64_t sequence); /* lock held, -1 absent */
void chat_mark_saved(ChatSession *s, size_t index, int64_t sequence);
void chat_enable_window(ChatSession *s);
/* Evict only saved prefix rows, idle and after persistence callbacks. Keep the
 * latest exchange even if it is larger than the normal 1 MiB retained tail. */
bool chat_trim_saved(ChatSession *s);
void chat_clear(ChatSession *s);

/* Call each frame on the UI thread: fires ChatMessageDone for finished
 * messages. Returns true if anything changed since the last poll. */
bool chat_poll(ChatSession *s);
void chat_on_message_done(ChatSession *s, ChatMessageDone fn, void *ctx);

/* One-shot notice for the status bar (e.g. "document was cut off"), or
 * NULL. The returned string is valid until the next call. */
const char *chat_take_notice(ChatSession *s);

void chat_lock(ChatSession *s);
void chat_unlock(ChatSession *s);
size_t chat_count(ChatSession *s);                    /* lock held */
const ChatMessage *chat_message(ChatSession *s, size_t i); /* lock held */
uint64_t chat_version(ChatSession *s);
uint64_t chat_version_locked(ChatSession *s); /* lock held; presentation snapshots */

/* Last request's owned history materialization (not documents, model context or
 * the UI/archive). Read on any thread. Counters reset when the session clears. */
typedef struct {
    size_t messages, content_bytes, scanned;
    bool limited;
} ChatHistoryStats;
ChatHistoryStats chat_history_stats(ChatSession *s);

/* ---- exposed for tests ------------------------------------------------ */

/* Allocated message slots, not just visible rows (lock held). */
size_t chat_allocated_slots(ChatSession *s);
/* Retained request/context string bytes; diagnostic scan, lock held. */
size_t chat_request_text_bytes(ChatSession *s);

/* Token counter: returns the number of tokens in `text`, or -1 if unknown
 * (the builder then falls back to a conservative estimate). */
typedef long (*ChatTokenCount)(const char *text, void *ctx);

/* Build the JSON request body for the conversation `msgs[0..n)` (the
 * in-progress assistant message excluded). Returns malloc'd JSON. If the
 * document had to be truncated, *doc_truncated is set. `count` may be NULL
 * to use the estimate only. */
char *chat_build_request(const ChatMessage *msgs, size_t n, const ChatContext *ctx, int ctx_tokens,
                         ChatTokenCount count, void *count_ctx, bool *doc_truncated);
/* Who is who, as stored facts (the author's settings). */
typedef struct {
    const char *author;    /* NULL/"" = unknown */
    const char *assistant; /* NULL/"" = unknown */
} ChatNames;
/* With the names stated as facts in the system message. Attached
 * documents go into the newest user message, before the author's words. */
char *chat_build_request_ex(const ChatMessage *msgs, size_t n, const ChatContext *ctx, const ChatNames *names, int ctx_tokens,
                            ChatTokenCount count, void *count_ctx, bool *doc_truncated);
/* Same request, with the exact first retained input-history index. On failure
 * first_history is n. Copying into a host snapshot does not mean a message
 * survived model-context trimming. */
char *chat_build_request_selected(const ChatMessage *msgs, size_t n, const ChatContext *ctx, const ChatNames *names, int ctx_tokens,
                                  ChatTokenCount count, void *count_ctx, bool *doc_truncated, size_t *first_history);

/* The request for an edit: an editor persona, the document as context, and
 * the instruction plus passage. No chat history (keeps a small model on
 * task). */
char *chat_build_edit_request(const char *instruction, const char *passage, const ChatContext *ctx, int ctx_tokens,
                              ChatTokenCount count, void *count_ctx, bool *doc_truncated);

/* Conservative token estimate (bytes / 1.5: denser than any text measured,
 * including code, numbers, emoji and CJK; see docs/REDTEAM-003). */
long chat_estimate_tokens(const char *text);

/* Parse one server-sent-event line. Appends content to *acc. Returns:
 * 1 = more to come, 0 = finished, 2 = finished at the length limit,
 * -1 = error (message in err). */
int chat_parse_sse_line(const char *line, char **acc, size_t *acc_len, char *err, size_t errlen);

/* Use the configured remote model for the next requests (NULL: the local
 * one). Only the author's explicit choice calls this; nothing falls back.
 * False while a reply is being written. */
bool chat_select_remote(ChatSession *s, const RemoteConfig *remote);
bool chat_remote_selected(ChatSession *s);

/* The author's name and the assistant's name, as stored facts (stated in
 * every request). Either may be "". */
void chat_set_names(ChatSession *s, const char *author, const char *assistant);

/* The author's model settings for chat requests (sampling and reply
 * length); NULL: the built-in defaults. */
typedef struct {
    int max_reply;
    double temperature, top_p;
    int top_k;
    double min_p, repeat_penalty, edit_temperature;
} ChatSampling;
void chat_set_sampling(ChatSession *s, const ChatSampling *sampling);
/* Exposed for tests: a request with the settings applied (malloc'd). */
char *chat_apply_sampling(const char *request, const ChatSampling *sampling, bool edit);

#endif
