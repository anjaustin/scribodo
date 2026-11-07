#define _DARWIN_C_SOURCE
#include "memory_gc.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool text(const char *s, size_t cap)
{
    return s && s[0] && strnlen(s, cap) < cap;
}
static bool scope_valid(const MemoryScope *s)
{
    return s && text(s->database, sizeof(s->database)) && text(s->id, sizeof(s->id)) &&
        text(s->project_id, 41) && text(s->conversation_id, sizeof(s->conversation_id));
}
static bool hash_valid(const char *s)
{
    if (!s || strnlen(s, 65) != 64) return false;
    for (size_t i = 0; i < 64; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    }
    return true;
}
/* Namespace component delimiters must be unambiguous. SQL-only cleanup does
 * not need this restriction, but a collection target must never alias another
 * database/project tuple by embedding the separator in a component. */
static bool namespace_component(const char *s, size_t cap)
{
    if (!text(s, cap) || strstr(s, "__")) return false;
    for (size_t i = 0; s[i]; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return false;
    }
    return true;
}
static bool lock_scope(Db *db, const MemoryScope *s, uint64_t *generation)
{
    DbRows r;
    if (!db_query(db, "SELECT generation FROM memory_scopes WHERE id=? AND project_id=?"
        " AND conversation_id=? AND DATABASE()=? FOR UPDATE",
        (DbArg[]){ DB_S(s->id), DB_S(s->project_id), DB_S(s->conversation_id), DB_S(s->database) }, 4, &r)) return false;
    bool ok = r.nrows == 1 && db_cell_u64(&r, 0, 0, generation) && *generation > 0 && *generation <= INT64_MAX;
    db_rows_free(&r);
    return ok;
}

/* Called only while the exact owning scope remains locked FOR UPDATE. Scope
 * controls use scope -> intent lock order too. The generation CAS documents the
 * cutoff and prevents a future refactor from clearing a newer dirty intent. */
static bool dequeue_if_clean(Db *db, const MemoryScope *scope, uint64_t generation)
{
    return db_run(db,
        "DELETE FROM memory_gc_scopes WHERE scope_id=? AND project_id=? AND conversation_id=?"
        " AND DATABASE()=? AND EXISTS(SELECT 1 FROM memory_scopes s WHERE s.id=memory_gc_scopes.scope_id"
        " AND s.generation=?)"
        " AND NOT EXISTS(SELECT 1 FROM memory_brief_inputs b WHERE b.scope_id=memory_gc_scopes.scope_id AND b.generation<?)"
        " AND NOT EXISTS(SELECT 1 FROM memory_briefs b WHERE b.scope_id=memory_gc_scopes.scope_id AND b.generation<?)"
        " AND NOT EXISTS(SELECT 1 FROM memory_brief_state b WHERE b.scope_id=memory_gc_scopes.scope_id AND b.generation<?)"
        " AND NOT EXISTS(SELECT 1 FROM memory_outbox b WHERE b.scope_id=memory_gc_scopes.scope_id AND b.generation<?)"
        " AND NOT EXISTS(SELECT 1 FROM memory_receipts b WHERE b.scope_id=memory_gc_scopes.scope_id AND b.generation<?)"
        " AND NOT EXISTS(SELECT 1 FROM memory_document_receipts b WHERE b.scope_id=memory_gc_scopes.scope_id AND b.generation<?)"
        " AND NOT EXISTS(SELECT 1 FROM memory_units b WHERE b.scope_id=memory_gc_scopes.scope_id AND b.generation<?)",
        (DbArg[]){ DB_S(scope->id), DB_S(scope->project_id), DB_S(scope->conversation_id), DB_S(scope->database),
                   DB_I(generation), DB_I(generation), DB_I(generation), DB_I(generation), DB_I(generation),
                   DB_I(generation), DB_I(generation), DB_I(generation) }, 12, NULL);
}

bool memory_gc_step(Db *db, const MemoryScope *s, size_t limit, MemoryGcStats *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!db || !out || !scope_valid(s) || !limit || limit > 64 || db_in_transaction(db) || !db_begin(db)) return false;
    MemoryGcStats st = {0};
    bool ok = lock_scope(db, s, &st.current_generation);
    const char *tables[] = { "memory_brief_inputs", "memory_briefs", "memory_brief_state", "memory_outbox",
                            "memory_receipts", "memory_document_receipts", "memory_units" };
    size_t *counts[] = { &st.brief_inputs, &st.briefs, &st.brief_states, &st.outbox,
                        &st.receipts, &st.document_receipts, &st.units };
    for (size_t i = 0; ok && i < 7 && st.total < limit; i++) {
        char sql[512];
        /* Unit deletion cannot cascade even one extra row beyond the budget. */
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE scope_id=? AND generation<?%s LIMIT %zu", tables[i],
                 i == 1 ? " AND NOT EXISTS(SELECT 1 FROM memory_brief_inputs b WHERE b.brief_id=memory_briefs.id)" :
                 i == 6 ? " AND NOT EXISTS(SELECT 1 FROM memory_outbox o WHERE o.unit_id=memory_units.id)"
                          " AND NOT EXISTS(SELECT 1 FROM memory_brief_inputs b WHERE b.unit_id=memory_units.id)" : "",
                 limit - st.total);
        uint64_t n = 0;
        ok = db_run(db, sql, (DbArg[]){ DB_S(s->id), DB_I(st.current_generation) }, 2, &n);
        if (ok) {
            *counts[i] = (size_t)n;
            st.total += (size_t)n;
        }
    }
    if (ok) ok = dequeue_if_clean(db, s, st.current_generation);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    if (ok) *out = st;
    return ok;
}

bool memory_gc_project_step(Db *db, const char *database, const char *project, size_t limit,
                            MemoryGcStats *out, MemoryScope *maintained_scope)
{
    if (out) memset(out, 0, sizeof(*out));
    if (maintained_scope) memset(maintained_scope, 0, sizeof(*maintained_scope));
    if (!db || !out || !maintained_scope || !namespace_component(database, 64) ||
        !namespace_component(project, 41) || !limit || limit > 64 || db_in_transaction(db)) return false;

    DbRows rows = {0};
    if (!db_query(db, "SELECT id FROM projects WHERE id=? AND DATABASE()=?",
                  (DbArg[]){ DB_S(project), DB_S(database) }, 2, &rows)) return false;
    bool ok = rows.nrows == 1;
    db_rows_free(&rows);
    if (!ok) return false;

    /* The queue's project-leading index avoids scanning unrelated projects or
     * source bodies. Composite FK ownership makes the join an exact scope. */
    ok = db_query(db, "SELECT s.conversation_id FROM memory_gc_scopes q FORCE INDEX(gc_project)"
                      " JOIN memory_scopes s ON s.id=q.scope_id AND s.project_id=q.project_id"
                      " AND s.conversation_id=q.conversation_id WHERE q.project_id=? AND DATABASE()=?"
                      " ORDER BY q.scope_id LIMIT 1",
                  (DbArg[]){ DB_S(project), DB_S(database) }, 2, &rows);
    if (!ok) return false;
    bool queued = rows.nrows != 0;
    if (!queued) {
        db_rows_free(&rows);
        ok = db_query(db, "SELECT conversation_id FROM memory_scopes WHERE project_id=? AND DATABASE()=?"
                          " ORDER BY conversation_id LIMIT 1",
                      (DbArg[]){ DB_S(project), DB_S(database) }, 2, &rows);
    }
    char conversation[40] = "";
    const char *value = NULL;
    if (ok && rows.nrows) {
        ok = rows.nrows == 1 && db_cell_text(&rows, 0, 0, &value) && strlen(value) < sizeof(conversation);
        if (ok) memcpy(conversation, value, strlen(value) + 1);
    }
    db_rows_free(&rows);
    if (!ok || !conversation[0]) return ok;

    MemoryScope scope;
    MemoryGcStats stats = {0};
    if (!memory_scope_get(db, project, conversation, &scope) || strcmp(scope.database, database)) return false;
    if (queued) {
        if (!memory_gc_step(db, &scope, limit, &stats)) return false;
        /* A scope control may have committed after cleanup; return its current
         * namespace owner, not the earlier admission snapshot. Its new intent
         * is already durable and cannot have been erased by our old cutoff. */
        if (!memory_scope_get(db, project, conversation, &scope) || strcmp(scope.database, database)) return false;
    } else {
        stats.current_generation = scope.generation;
    }
    *out = stats;
    *maintained_scope = scope;
    return true;
}

bool memory_gc_namespace_enqueue(Db *db, const MemoryScope *s, const char *fp, int version)
{
    if (!db || !scope_valid(s) || !namespace_component(s->database, sizeof(s->database)) ||
        !namespace_component(s->project_id, 41) || !s->generation || s->generation > INT64_MAX ||
        !hash_valid(fp) || (version != 1 && version != 2)) return false;
    bool own = !db_in_transaction(db);
    if (own && !db_begin(db)) return false;
    uint64_t generation = 0;
    bool ok = lock_scope(db, s, &generation) && s->generation <= generation;
    if (ok) ok = db_run(db,"INSERT INTO memory_retired_namespaces"
        "(database_name,project_id,scope_id,conversation_id,generation,namespace_version,embedding_fingerprint)"
        " VALUES(?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE id=id",
        (DbArg[]){ DB_S(s->database), DB_S(s->project_id), DB_S(s->id), DB_S(s->conversation_id),
                  DB_I(s->generation), DB_I(version), DB_S(fp) }, 7, NULL);
    if (own) {
        if (ok) ok = db_commit(db);
        else db_rollback(db);
    }
    return ok;
}

static bool lease_valid(const MemoryNamespaceLease *l)
{
    return l && l->id && l->id <= INT64_MAX && l->fence && l->fence <= INT64_MAX &&
           text(l->owner, sizeof(l->owner)) && scope_valid(&l->scope) &&
           namespace_component(l->scope.database, sizeof(l->scope.database)) &&
           namespace_component(l->scope.project_id, 41) && l->scope.generation &&
           l->scope.generation <= INT64_MAX && hash_valid(l->embedding_fingerprint) &&
           (l->namespace_version == 1 || l->namespace_version == 2);
}
/* Disabled scopes cannot write, and every future enable must jump above the
 * committed project floor. Never infer active writers from vector payload. */
#define RETIRED " NOT EXISTS(SELECT 1 FROM memory_scopes s WHERE s.project_id=j.project_id AND s.enabled=1 AND s.generation<=j.generation)"
#define LIVE "j.id=? AND j.fence=? AND j.owner=? AND j.database_name=? AND DATABASE()=j.database_name" \
    " AND j.project_id=? AND j.scope_id=? AND j.conversation_id=? AND j.generation=?" \
    " AND j.namespace_version=? AND j.embedding_fingerprint=? AND j.state='leased' AND j.lease_until>NOW(6)"
static void lease_args(const MemoryNamespaceLease *l, DbArg a[10])
{
    a[0] = DB_I(l->id);
    a[1] = DB_I(l->fence);
    a[2] = DB_S(l->owner);
    a[3] = DB_S(l->scope.database);
    a[4] = DB_S(l->scope.project_id);
    a[5] = DB_S(l->scope.id);
    a[6] = DB_S(l->scope.conversation_id);
    a[7] = DB_I(l->scope.generation);
    a[8] = DB_I(l->namespace_version);
    a[9] = DB_S(l->embedding_fingerprint);
}
bool memory_gc_namespace_claim_project(Db *db, const char *database, const char *project, const char *owner,
                                       MemoryNamespaceLease *out, bool *claimed)
{
    if (out) memset(out, 0, sizeof(*out));
    if (claimed) *claimed = false;
    if (!db || !out || !claimed || !namespace_component(database, 64) || !namespace_component(project, 41) ||
        !text(owner, 64) || db_in_transaction(db) || !db_begin(db)) return false;
    MemoryScope authority = {0};
    snprintf(authority.database, sizeof(authority.database), "%s", database);
    snprintf(authority.project_id, sizeof(authority.project_id), "%s", project);
    const MemoryScope *s = &authority;
    DbRows project_row = {0};
    bool ok = db_query(db, "SELECT id FROM projects WHERE id=? AND DATABASE()=? FOR UPDATE",
                       (DbArg[]){ DB_S(project), DB_S(database) }, 2, &project_row);
    if (ok) ok = project_row.nrows == 1;
    db_rows_free(&project_row);
    /* A process can die after its third network request, before ack. Retire a
     * bounded expired terminal lease instead of presenting it as in flight. */
    if (ok) ok = db_run(db, "UPDATE memory_retired_namespaces SET state='failed',lease_until=NULL,"
        "retry_after=TIMESTAMPADD(SECOND,300,NOW(6))"
        " WHERE database_name=? AND DATABASE()=? AND project_id=? AND state='leased' AND attempts>=3"
        " AND lease_until<=NOW(6) ORDER BY id LIMIT 1",
        (DbArg[]){ DB_S(s->database), DB_S(s->database), DB_S(s->project_id) }, 3, NULL);
    /* Tombstones are durable authority, not one-shot delivery receipts. A
     * creator can crash after PUT but before its post-create fence/DELETE.
     * Re-audit only these exact known targets, at most one per worker step;
     * never discover arbitrary collections or hold SQL locks across HTTP. */
    if (ok) ok = db_run(db, "UPDATE memory_retired_namespaces SET state='pending',attempts=0"
        " WHERE database_name=? AND DATABASE()=? AND project_id=? AND state IN ('done','failed')"
        " AND retry_after<=NOW(6) ORDER BY retry_after,id LIMIT 1",
        (DbArg[]){ DB_S(s->database), DB_S(s->database), DB_S(s->project_id) }, 3, NULL);
    DbRows r = {0};
    if (ok) ok = db_query(db, "SELECT j.id,j.fence,j.generation,j.namespace_version,j.embedding_fingerprint,"
        "j.scope_id,j.conversation_id FROM memory_retired_namespaces j WHERE j.database_name=?"
        " AND DATABASE()=? AND j.project_id=? AND j.state IN ('pending','leased') AND j.attempts<3"
        " AND (j.lease_until IS NULL OR j.lease_until<=NOW(6))"
        " AND (j.retry_after IS NULL OR j.retry_after<=NOW(6)) AND" RETIRED " ORDER BY j.id LIMIT 1 FOR UPDATE",
        (DbArg[]){ DB_S(s->database), DB_S(s->database), DB_S(s->project_id) }, 3, &r);
    MemoryNamespaceLease l = {0};
    if (ok && r.nrows) {
        uint64_t version;
        l.scope = *s;
        ok = r.nrows == 1 && db_cell_u64(&r, 0, 0, &l.id) && db_cell_u64(&r, 0, 1, &l.fence) &&
             l.fence < INT64_MAX && db_cell_u64(&r, 0, 2, &l.scope.generation) &&
             db_cell_u64(&r, 0, 3, &version) && version <= 2;
        if (ok) {
            l.namespace_version = (int)version;
            ++l.fence;
            snprintf(l.owner, sizeof(l.owner), "%s", owner);
            snprintf(l.embedding_fingerprint, sizeof(l.embedding_fingerprint), "%s", db_cell(&r, 0, 4));
            snprintf(l.scope.id, sizeof(l.scope.id), "%s", db_cell(&r, 0, 5));
            snprintf(l.scope.conversation_id, sizeof(l.scope.conversation_id), "%s", db_cell(&r, 0, 6));
            ok = lease_valid(&l) && db_run(db, "UPDATE memory_retired_namespaces SET state='leased',owner=?,fence=fence+1,"
                "attempts=attempts+1,lease_until=TIMESTAMPADD(SECOND,30,NOW(6)),retry_after=NULL WHERE id=?",
                (DbArg[]){ DB_S(owner), DB_I(l.id) }, 2, NULL);
        }
    }
    db_rows_free(&r);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    if (ok && l.id) {
        *out = l;
        *claimed = true;
    }
    return ok;
}
bool memory_gc_namespace_claim(Db *db, const MemoryScope *s, const char *owner,
                               MemoryNamespaceLease *out, bool *claimed)
{
    if (!scope_valid(s)) {
        if (out) memset(out, 0, sizeof(*out));
        if (claimed) *claimed = false;
        return false;
    }
    return memory_gc_namespace_claim_project(db, s->database, s->project_id, owner, out, claimed);
}

bool memory_gc_namespace_ack(Db *db, const MemoryNamespaceLease *l, bool delivered)
{
    if (!db || !lease_valid(l) || db_in_transaction(db)) return false;
    DbArg a[10];
    lease_args(l, a);
    uint64_t n = 0;
    const char *sql = delivered
        ? "UPDATE memory_retired_namespaces j SET state='done',lease_until=NULL,"
          "retry_after=TIMESTAMPADD(SECOND,300,NOW(6)) WHERE " LIVE " AND" RETIRED
          " AND EXISTS(SELECT 1 FROM memory_namespace_floor f WHERE f.project_id=j.project_id"
          " AND f.generation_floor>=j.generation)"
        : "UPDATE memory_retired_namespaces j SET state=IF(attempts>=3,'failed','pending'),lease_until=NULL,"
           "retry_after=TIMESTAMPADD(SECOND,IF(attempts>=3,300,30),NOW(6)) WHERE " LIVE;
    return db_run(db, sql, a, 10, &n) && n == 1;
}

bool memory_gc_namespace_prepare(Db *db, const MemoryNamespaceLease *l)
{
    if (!db || !lease_valid(l) || l->scope.generation >= INT64_MAX || db_in_transaction(db) || !db_begin(db)) return false;
    DbRows r = {0};
    bool ok = db_query(db, "SELECT id FROM projects WHERE id=? AND DATABASE()=? FOR UPDATE",
                       (DbArg[]){ DB_S(l->scope.project_id), DB_S(l->scope.database) }, 2, &r);
    if (ok) ok = r.nrows == 1;
    db_rows_free(&r);
    /* Scope creation/enable/rebuild takes this same project lock before any
     * generation change. It therefore stabilizes eligibility without fetching
     * or locking an unbounded list of every conversation/scope in the project.
     * Ordinary appends cannot enable a scope or lower its generation. */
    DbArg a[10];
    lease_args(l, a);
    if (ok) ok = db_query(db, "SELECT j.id FROM memory_retired_namespaces j WHERE " LIVE " AND" RETIRED
                             " FOR UPDATE", a, 10, &r);
    if (ok) ok = r.nrows == 1;
    db_rows_free(&r);
    if (ok) ok = db_run(db, "INSERT INTO memory_namespace_floor(project_id,generation_floor) VALUES(?,?)"
                           " ON DUPLICATE KEY UPDATE generation_floor=GREATEST(generation_floor,VALUES(generation_floor))",
                       (DbArg[]){ DB_S(l->scope.project_id), DB_I(l->scope.generation) }, 2, NULL);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    return ok;
}

bool memory_gc_namespace_admit(Db *db, const MemoryScope *s, bool *allowed, bool *retired)
{
    if (allowed) *allowed = false;
    if (retired) *retired = false;
    if (!db || !allowed || !retired || !scope_valid(s) || !s->generation || s->generation > INT64_MAX ||
        !namespace_component(s->database, sizeof(s->database)) || !namespace_component(s->project_id, 41) ||
        db_in_transaction(db)) return false;
    DbRows r = {0};
    bool ok = db_query(db, "SELECT COALESCE(f.generation_floor,0),EXISTS(SELECT 1 FROM memory_scopes s"
        " WHERE s.project_id=p.id AND s.id=? AND s.conversation_id=? AND s.enabled=1 AND s.generation=?"
        " AND s.history_epoch=? AND s.policy_epoch=?) FROM projects p"
        " LEFT JOIN memory_namespace_floor f ON f.project_id=p.id WHERE p.id=? AND DATABASE()=?",
        (DbArg[]){ DB_S(s->id), DB_S(s->conversation_id), DB_I(s->generation), DB_I(s->history_epoch),
                  DB_I(s->policy_epoch), DB_S(s->project_id), DB_S(s->database) }, 7, &r);
    uint64_t floor = 0, eligible = 0;
    if (ok) ok = r.nrows == 1 && db_cell_u64(&r, 0, 0, &floor) && db_cell_u64(&r, 0, 1, &eligible) && eligible <= 1;
    if (ok) {
        *retired = s->generation <= floor;
        *allowed = eligible && !*retired;
    }
    db_rows_free(&r);
    return ok;
}
