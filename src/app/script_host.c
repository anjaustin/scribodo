/* The scripted host's completeness check (app/script.h), kept apart from the script runner so it links headless. */
#include "app/script.h"

#include <string.h>

#define MEMBER(name) { #name, offsetof(ScriptHost, name) }
static const struct {
    const char *name;
    size_t offset;
} members[] = {
    MEMBER(editor), MEMBER(explorer_w), MEMBER(observer_w), MEMBER(editor_rect), MEMBER(status), MEMBER(open),
    MEMBER(drop), MEMBER(ctx), MEMBER(chat), MEMBER(element), MEMBER(model_ready), MEMBER(record_text),
    MEMBER(observer_enable), MEMBER(observer_idle), MEMBER(findings_text), MEMBER(attached_text),
    MEMBER(explorer_text), MEMBER(observer_status), MEMBER(history_cache_bytes), MEMBER(store_offline),
    MEMBER(history_busy), MEMBER(history_hold), MEMBER(history_worker_busy), MEMBER(store_restart),
    MEMBER(memory_settled), MEMBER(memory_enabled), MEMBER(memory_sources), MEMBER(memory_documents_settled),
    MEMBER(memory_briefs_ready), MEMBER(recall_text), MEMBER(recall_busy), MEMBER(model_status),
};
#undef MEMBER

_Static_assert(sizeof(members) / sizeof(members[0]) == SCRIPT_HOST_MEMBERS,
               "every ScriptHost member is named in script_host.c");
_Static_assert(sizeof(ScriptHost) == SCRIPT_HOST_MEMBERS * sizeof(void *),
               "ScriptHost gained or lost a member: update SCRIPT_HOST_MEMBERS and the list in script_host.c");

const char *script_host_missing(const ScriptHost *h)
{
    static const unsigned char null_bits[sizeof(void *)];
    for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++)
        if (!memcmp((const unsigned char *)h + members[i].offset, null_bits, sizeof(null_bits))) return members[i].name;
    return NULL;
}
