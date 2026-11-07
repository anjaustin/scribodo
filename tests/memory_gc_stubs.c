/* TEST ONLY: explicit authority double for the SQL-less vector fixtures.
 * Never link this translation unit into the application or SQL GC tests.
 * Fixed storage also keeps allocation-fault sweeps independent of authority. */
#include "app/memory_gc.h"
#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool authorized, registered, prepared;
    MemoryScope scope;
    char fingerprint[65];
    MemoryNamespaceLease lease;
    uint64_t floor;
    unsigned acknowledgments;
} authority;

static bool same_scope(const MemoryScope *a, const MemoryScope *b)
{
    return a && b && !strncmp(a->database, b->database, sizeof(a->database)) &&
           !strncmp(a->project_id, b->project_id, sizeof(a->project_id)) &&
           !strncmp(a->id, b->id, sizeof(a->id)) &&
           !strncmp(a->conversation_id, b->conversation_id, sizeof(a->conversation_id)) &&
           a->generation == b->generation && a->history_epoch == b->history_epoch &&
           a->policy_epoch == b->policy_epoch;
}

static bool same_lease(const MemoryNamespaceLease *a, const MemoryNamespaceLease *b)
{
    return a && b && same_scope(&a->scope, &b->scope) && a->id == b->id && a->fence == b->fence &&
           a->namespace_version == b->namespace_version && !strncmp(a->owner, b->owner, sizeof(a->owner)) &&
           !strncmp(a->embedding_fingerprint, b->embedding_fingerprint, sizeof(a->embedding_fingerprint));
}

void memory_gc_test_reset(void)
{
    pthread_mutex_lock(&lock);
    memset(&authority, 0, sizeof(authority));
    pthread_mutex_unlock(&lock);
}

void memory_gc_test_authorize(const MemoryScope *scope, const char *fingerprint)
{
    pthread_mutex_lock(&lock);
    memset(&authority, 0, sizeof(authority));
    if (scope && fingerprint && strlen(fingerprint) == 64) {
        authority.scope = *scope;
        memcpy(authority.fingerprint, fingerprint, sizeof(authority.fingerprint));
        authority.authorized = true;
    }
    pthread_mutex_unlock(&lock);
}

void memory_gc_test_register(const MemoryNamespaceLease *lease)
{
    pthread_mutex_lock(&lock);
    authority.registered = lease != NULL;
    authority.prepared = false;
    if (lease) authority.lease = *lease;
    pthread_mutex_unlock(&lock);
}

void memory_gc_test_advance_floor(uint64_t floor)
{
    pthread_mutex_lock(&lock);
    if (floor > authority.floor) authority.floor = floor;
    pthread_mutex_unlock(&lock);
}

unsigned memory_gc_test_ack_count(void)
{
    pthread_mutex_lock(&lock);
    unsigned count = authority.acknowledgments;
    pthread_mutex_unlock(&lock);
    return count;
}

bool memory_gc_namespace_admit(Db *db, const MemoryScope *scope, bool *allowed, bool *retired)
{
    (void)db; /* NULL is accepted ONLY by this explicitly authorized test double. */
    if (allowed) *allowed = false;
    if (retired) *retired = false;
    if (!allowed || !retired || !scope) return false;
    pthread_mutex_lock(&lock);
    bool ok = authority.authorized && same_scope(scope, &authority.scope);
    if (ok) {
        *retired = scope->generation <= authority.floor;
        *allowed = authority.scope.enabled && !*retired;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

bool memory_gc_namespace_prepare(Db *db, const MemoryNamespaceLease *lease)
{
    (void)db;
    pthread_mutex_lock(&lock);
    bool ok = authority.authorized && authority.registered && same_lease(lease, &authority.lease) &&
              !strncmp(lease->scope.database, authority.scope.database, sizeof(lease->scope.database)) &&
              !strncmp(lease->scope.project_id, authority.scope.project_id, sizeof(lease->scope.project_id)) &&
              !strncmp(lease->embedding_fingerprint, authority.fingerprint, sizeof(lease->embedding_fingerprint)) &&
              (!authority.scope.enabled || authority.scope.generation > lease->scope.generation);
    if (ok) {
        if (lease->scope.generation > authority.floor) authority.floor = lease->scope.generation;
        authority.prepared = true;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

bool memory_gc_namespace_ack(Db *db, const MemoryNamespaceLease *lease, bool delivered)
{
    (void)db;
    pthread_mutex_lock(&lock);
    bool ok = authority.authorized && authority.registered && authority.prepared &&
              same_lease(lease, &authority.lease);
    if (ok && delivered) ++authority.acknowledgments;
    pthread_mutex_unlock(&lock);
    return ok;
}

bool memory_gc_step(Db *db, const MemoryScope *scope, size_t limit, MemoryGcStats *stats)
{
    (void)db;
    (void)scope;
    if (stats) memset(stats, 0, sizeof(*stats));
    return stats && limit > 0 && limit <= 64;
}

bool memory_gc_project_step(Db *db, const char *database, const char *project, size_t limit,
                            MemoryGcStats *stats, MemoryScope *scope)
{
    (void)db; (void)database; (void)project;
    if (stats) memset(stats, 0, sizeof(*stats));
    if (scope) memset(scope, 0, sizeof(*scope));
    return stats && scope && limit && limit <= 64;
}

bool memory_gc_namespace_enqueue(Db *db, const MemoryScope *scope, const char *fingerprint, int version)
{
    (void)db;
    pthread_mutex_lock(&lock);
    bool ok = authority.authorized && same_scope(scope, &authority.scope) && version == 2 &&
              fingerprint && !strcmp(fingerprint, authority.fingerprint);
    pthread_mutex_unlock(&lock);
    return ok; /* Queue persistence itself is exercised by the real SQL suite. */
}

bool memory_gc_namespace_claim_project(Db *db, const char *database, const char *project,
                                       const char *owner, MemoryNamespaceLease *lease, bool *claimed)
{
    (void)database; (void)project;
    return memory_gc_namespace_claim(db, NULL, owner, lease, claimed);
}

bool memory_gc_namespace_claim(Db *db, const MemoryScope *scope, const char *owner,
                               MemoryNamespaceLease *lease, bool *claimed)
{
    (void)db;
    (void)scope;
    (void)owner;
    if (lease) memset(lease, 0, sizeof(*lease));
    if (claimed) *claimed = false;
    return lease && claimed;
}
