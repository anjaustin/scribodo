#include "app/draft_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/doc_io.h"

uint64_t draft_text_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = (h ^ *p) * 1099511628211ULL;
    return h;
}

void draft_controller_init(DraftController *draft)
{
    if (draft) memset(draft, 0, sizeof(*draft));
}

void draft_controller_discard(DraftController *draft)
{
    if (draft->path[0]) unlink(draft->path);
    draft->path[0] = draft->doc[0] = '\0';
    draft->hash = 0;
}

bool draft_controller_tick(DraftController *draft, const char *doc, bool dirty, double now, DraftTextFn text, void *ctx)
{
    if (!doc || !dirty || (draft->doc[0] && strcmp(draft->doc, doc) != 0)) {
        if (draft->path[0]) draft_controller_discard(draft);
        return false;
    }
    if (now - draft->at < DRAFT_INTERVAL_S) return false;
    draft->at = now;
    char *buffer = text(ctx);
    if (!buffer) return false;
    bool wrote = false;
    uint64_t h = draft_text_hash(buffer);
    if (h != draft->hash) {
        char out[PATH_MAX];
        if (doc_write_draft(doc, buffer, strlen(buffer), draft->path, out, sizeof(out))) {
            snprintf(draft->path, sizeof(draft->path), "%s", out);
            snprintf(draft->doc, sizeof(draft->doc), "%s", doc);
            draft->hash = h;
            wrote = true;
        }
    }
    free(buffer);
    return wrote;
}
