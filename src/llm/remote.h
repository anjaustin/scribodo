/* The optional remote model (SOW §4.4, §7, acceptance 8).
 *
 * One OpenAI-compatible chat-completions provider, off unless the author
 * configures it in ~/Library/Application Support/Scribodo/remote.json (or
 * $SCRIBODO_REMOTE_CONFIG):
 *
 *   { "name": "Frontier", "base_url": "https://api.example.com/v1",
 *     "model": "model-name", "api_key_file": "/path/to/key",
 *     "context_tokens": 32000 }
 *
 * The key file must be private (0600 or stricter); the key is never logged.
 * HTTPS is required, except to this machine (http://127.0.0.1, tests).
 * The app uses it only when the author selects it, shows that project
 * content leaves the device while it's selected, and never falls back from
 * local to remote (or back) on its own. The observer always stays local.
 */
#ifndef SCRIBODO_REMOTE_H
#define SCRIBODO_REMOTE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "llm/http.h"

/* A provider is outside the trust boundary and is not required to honour
 * max_tokens. Bound both streamed wire data and non-streamed response bodies. */
#define REMOTE_MAX_RESPONSE_BYTES (16u * 1024u * 1024u)

typedef struct {
    char name[64];
    char base_url[512]; /* without a trailing slash */
    char host[256];     /* shown in the data-sharing indicator */
    char model[128];
    char key[512];
    int context_tokens;
} RemoteConfig;

/* False with a reason if there's no usable configuration. `err` is empty
 * when there is simply no config file (the normal case). */
bool remote_load(RemoteConfig *c, char *err, size_t errlen);
/* Parse a config (tests): JSON text, the key file's contents read from the
 * path it names. */
bool remote_parse(const char *json, RemoteConfig *c, char *err, size_t errlen);

/* POST `body` to base_url + path, streaming the response body line by line
 * to `on_line` (or whole into result->body when on_line is NULL). */
bool remote_request(const RemoteConfig *c, const char *path, const char *body, HttpLineFn on_line, void *ctx,
                    atomic_bool *cancel, int timeout_ms, HttpResult *result);

/* The local request, adapted: llama-server extras removed, the model named. */
char *remote_adapt_request(const RemoteConfig *c, const char *local_request);

#endif
