/* Conversations in the project store: messages in sequence, and the
 * documents each user message referenced (by id). Called by chat_store.c
 * for projects in the store. */
#define _DARWIN_C_SOURCE
#include "store_backends.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* The conversation exists, belongs to this project, and is locked. */
static bool own_conversation(Db *db, const char *project_id, const char *id)
{
    DbRows r;
    if (!db_query(db, "SELECT project_id FROM conversations WHERE id = ? FOR UPDATE", (DbArg[1]){ DB_S(id) }, 1, &r))
        return false;
    bool ok = r.nrows == 1 && db_cell(&r, 0, 0) && !strcmp(db_cell(&r, 0, 0), project_id);
    db_rows_free(&r);
    return ok;
}

bool sdb_chat_append(Db *db, const char *project_id, const char *id, const char *title, const ChatMessage *m)
{
    double t = now_s();
    if (!db_begin(db)) return false;
    DbArg c[5] = { DB_S(id), DB_S(project_id), DB_S(title && title[0] ? title : "Chat"), DB_D(t), DB_D(t) };
    bool ok = db_run(db, "INSERT IGNORE INTO conversations (id, project_id, title, created_at, updated_at) VALUES (?,?,?,?,?)",
                     c, 5, NULL) &&
              own_conversation(db, project_id, id);
    int seq = 0;
    DbRows r;
    if (ok && db_query(db, "SELECT COALESCE(MAX(seq), -1) + 1 FROM messages WHERE conversation_id = ?",
                       (DbArg[1]){ DB_S(id) }, 1, &r)) {
        seq = r.nrows && db_cell(&r, 0, 0) ? atoi(db_cell(&r, 0, 0)) : 0;
        db_rows_free(&r);
    } else {
        ok = false;
    }
    DbArg a[9] = { DB_S(id), DB_I(seq), DB_S(m->role == CHAT_USER ? "user" : "assistant"), DB_S(m->content ? m->content : ""),
                   DB_I(m->error ? 1 : 0), DB_I(m->is_edit ? 1 : 0), DB_I(m->truncated ? 1 : 0), DB_D(t),
                   DB_S(m->via && m->via[0] ? m->via : NULL) };
    ok = ok && db_run(db,
                      "INSERT INTO messages (conversation_id, seq, role, content, error, is_edit, truncated, created_at, via)"
                      " VALUES (?,?,?,?,?,?,?,?,?)",
                      a, 9, NULL);
    /* The documents this message was sent with. */
    if (ok && m->docs && m->docs[0]) {
        char *copy = strdup(m->docs);
        for (char *save = NULL, *nm = copy ? strtok_r(copy, "\n", &save) : NULL; ok && nm; nm = strtok_r(NULL, "\n", &save)) {
            char doc[40];
            if (!store_document_id(project_id, nm, true, doc, sizeof(doc))) continue;
            DbArg d[3] = { DB_S(doc), DB_S(id), DB_I(seq) };
            ok = db_run(db,
                        "INSERT IGNORE INTO message_documents (message_id, document_id)"
                        " SELECT id, ? FROM messages WHERE conversation_id = ? AND seq = ?",
                        d, 3, NULL);
        }
        free(copy);
    }
    ok = ok && db_run(db, "UPDATE conversations SET updated_at = ? WHERE id = ?", (DbArg[2]){ DB_D(t), DB_S(id) }, 2, NULL);
    if (ok) ok = db_commit(db);
    else db_rollback(db);
    return ok;
}

size_t sdb_chat_list(Db *db, const char *project_id, ChatSummary **out)
{
    *out = NULL;
    DbRows r;
    if (!db_query(db, "SELECT id, title, updated_at, observer_ignore FROM conversations WHERE project_id = ? ORDER BY updated_at DESC",
                  (DbArg[1]){ DB_S(project_id) }, 1, &r))
        return 0;
    ChatSummary *list = r.nrows ? calloc(r.nrows, sizeof(ChatSummary)) : NULL;
    size_t n = 0;
    for (size_t i = 0; list && i < r.nrows; i++) {
        ChatSummary *c = &list[n++];
        snprintf(c->id, sizeof(c->id), "%s", db_cell(&r, i, 0));
        snprintf(c->title, sizeof(c->title), "%s", db_cell(&r, i, 1) ? db_cell(&r, i, 1) : "Chat");
        c->updated = (time_t)atof(db_cell(&r, i, 2) ? db_cell(&r, i, 2) : "0");
        c->observer_ignore = db_cell(&r, i, 3) && atoi(db_cell(&r, i, 3)) == 1;
    }
    db_rows_free(&r);
    *out = list;
    return n;
}

bool sdb_chat_load(Db *db, const char *project_id, const char *id, ChatMessage **msgs, size_t *n)
{
    *msgs = NULL;
    *n = 0;
    DbRows r, d;
    DbArg k[2] = { DB_S(id), DB_S(project_id) };
    if (!db_query(db,
                  "SELECT m.seq, m.role, m.content, m.error, m.is_edit, m.truncated, m.via, m.created_at FROM messages m"
                  " JOIN conversations c ON c.id = m.conversation_id WHERE m.conversation_id = ? AND c.project_id = ?"
                  " ORDER BY m.seq",
                  k, 2, &r))
        return false;
    if (!db_query(db,
                  "SELECT m.seq, doc.name FROM message_documents md JOIN messages m ON m.id = md.message_id"
                  " JOIN documents doc ON doc.id = md.document_id JOIN conversations c ON c.id = m.conversation_id"
                  " WHERE m.conversation_id = ? AND c.project_id = ? ORDER BY m.seq, doc.name",
                  k, 2, &d)) {
        db_rows_free(&r);
        return false;
    }
    ChatMessage *list = r.nrows ? calloc(r.nrows, sizeof(ChatMessage)) : NULL;
    size_t k2 = 0;
    for (size_t i = 0; list && i < r.nrows; i++) {
        ChatMessage *m = &list[*n];
        m->role = db_cell(&r, i, 1) && !strcmp(db_cell(&r, i, 1), "user") ? CHAT_USER : CHAT_ASSISTANT;
        m->content = strdup(db_cell(&r, i, 2) ? db_cell(&r, i, 2) : "");
        m->error = db_cell(&r, i, 3) && atoi(db_cell(&r, i, 3));
        m->is_edit = db_cell(&r, i, 4) && atoi(db_cell(&r, i, 4));
        m->truncated = db_cell(&r, i, 5) && atoi(db_cell(&r, i, 5));
        m->via = db_cell(&r, i, 6) && db_cell(&r, i, 6)[0] ? strdup(db_cell(&r, i, 6)) : NULL;
        m->ts = db_cell(&r, i, 7) ? atof(db_cell(&r, i, 7)) : 0;
        /* The documents' *current* names: renames are followed for free. */
        int seq = atoi(db_cell(&r, i, 0));
        size_t len = 1;
        for (size_t j = k2; j < d.nrows && atoi(db_cell(&d, j, 0)) == seq; j++) len += strlen(db_cell(&d, j, 1)) + 1;
        if (len > 1 && (m->docs = malloc(len)) != NULL) {
            m->docs[0] = '\0';
            for (; k2 < d.nrows && atoi(db_cell(&d, k2, 0)) == seq; k2++) {
                if (m->docs[0]) strcat(m->docs, "\n");
                strcat(m->docs, db_cell(&d, k2, 1));
            }
        }
        while (k2 < d.nrows && atoi(db_cell(&d, k2, 0)) <= seq) k2++;
        if (m->content) (*n)++;
    }
    bool ok = list != NULL || r.nrows == 0;
    db_rows_free(&r);
    db_rows_free(&d);
    *msgs = list;
    return ok;
}

bool sdb_chat_set_observer(Db *db, const char *project_id, const char *id, bool read)
{
    uint64_t changed = 0;
    DbArg a[3] = { DB_I(read ? 0 : 1), DB_S(id), DB_S(project_id) };
    return db_run(db, "UPDATE conversations SET observer_ignore = ? WHERE id = ? AND project_id = ?", a, 3, &changed);
}

bool sdb_chat_rename(Db *db, const char *project_id, const char *id, const char *title)
{
    uint64_t changed = 0;
    DbArg a[4] = { DB_S(title), DB_D(now_s()), DB_S(id), DB_S(project_id) };
    return db_run(db, "UPDATE conversations SET title = ?, updated_at = ? WHERE id = ? AND project_id = ?", a, 4, &changed) &&
           changed == 1;
}
