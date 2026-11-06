/* Conversations in the project store: messages in sequence, and the
 * documents each user message referenced (by id). Called by chat_store.c
 * for projects in the store. */
#define _DARWIN_C_SOURCE
#include "store_backends.h"

#include <limits.h>
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

typedef struct {
    const char *id;
    const char *title;
    double updated;
    bool observer_ignore;
} ChatSummaryRow;

typedef struct {
    int64_t seq;
    const char *role;
    const char *content;
    bool error;
    bool is_edit;
    bool truncated;
    const char *via;
    double created_at;
} ChatMessageRow;

static bool bool_cell(const DbRows *rows, size_t row, size_t col, bool *out)
{
    int64_t value;
    if (!db_cell_i64(rows, row, col, &value) || (value != 0 && value != 1)) return false;
    *out = value == 1;
    return true;
}

static bool chat_summary_row(const DbRows *rows, size_t row, ChatSummaryRow *out)
{
    bool ok = db_rows_has_columns(rows, 4) && db_cell_text(rows, row, 0, &out->id) &&
           db_cell_text(rows, row, 1, &out->title) && db_cell_double(rows, row, 2, &out->updated) &&
           bool_cell(rows, row, 3, &out->observer_ignore);
    return ok && out->updated >= 0 && out->updated < 0x1p63 &&
           strlen(out->id) < sizeof(((ChatSummary *)0)->id) && strlen(out->title) < sizeof(((ChatSummary *)0)->title);
}

static bool chat_message_row(const DbRows *rows, size_t row, ChatMessageRow *out)
{
    bool ok = db_rows_has_columns(rows, 8) && db_cell_i64(rows, row, 0, &out->seq) &&
           db_cell_text(rows, row, 1, &out->role) && db_cell_text(rows, row, 2, &out->content) &&
           bool_cell(rows, row, 3, &out->error) && bool_cell(rows, row, 4, &out->is_edit) &&
           bool_cell(rows, row, 5, &out->truncated) && db_cell_nullable(rows, row, 6, &out->via) &&
           db_cell_double(rows, row, 7, &out->created_at);
    return ok && out->seq >= 0 && out->created_at >= 0 && out->created_at < 0x1p63;
}

static bool chat_document_row(const DbRows *rows, size_t row, int64_t *seq, const char **name)
{
    return db_rows_has_columns(rows, 2) && db_cell_i64(rows, row, 0, seq) && *seq >= 0 &&
           db_cell_text(rows, row, 1, name);
}

static void chat_messages_free_partial(ChatMessage *messages, size_t count)
{
    if (!messages) return;
    for (size_t i = 0; i < count; i++) {
        free(messages[i].content);
        free(messages[i].docs);
        free(messages[i].via);
    }
    free(messages);
}

/* The conversation exists, belongs to this project, and is locked. */
static bool own_conversation(Db *db, const char *project_id, const char *id)
{
    DbRows r;
    if (!db_query(db, "SELECT project_id FROM conversations WHERE id = ? FOR UPDATE", (DbArg[1]){ DB_S(id) }, 1, &r))
        return false;
    const char *owner;
    bool ok = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_text(&r, 0, 0, &owner) && !strcmp(owner, project_id);
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
        int64_t next;
        ok = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_i64(&r, 0, 0, &next) && next >= 0 && next <= INT_MAX;
        if (ok) seq = (int)next;
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
        if (!copy) ok = false;
        for (char *save = NULL, *nm = copy ? strtok_r(copy, "\n", &save) : NULL; ok && nm; nm = strtok_r(NULL, "\n", &save)) {
            char doc[40];
            if (!store_document_id_db(db, project_id, nm, true, doc, sizeof(doc))) continue;
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

bool sdb_chat_list_checked(Db *db, const char *project_id, ChatSummary **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    DbRows r;
    if (!db_query(db, "SELECT id, title, updated_at, observer_ignore FROM conversations WHERE project_id = ? ORDER BY updated_at DESC",
                  (DbArg[1]){ DB_S(project_id) }, 1, &r))
        return false;
    bool ok = db_rows_has_columns(&r, 4);
    ChatSummary *list = ok && r.nrows ? calloc(r.nrows, sizeof(ChatSummary)) : NULL;
    if (r.nrows && !list) {
        db_rows_free(&r);
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; ok && list && i < r.nrows; i++) {
        ChatSummaryRow row;
        ok = chat_summary_row(&r, i, &row);
        if (!ok) break;
        ChatSummary *c = &list[n++];
        snprintf(c->id, sizeof(c->id), "%s", row.id);
        snprintf(c->title, sizeof(c->title), "%s", row.title);
        c->updated = (time_t)row.updated;
        c->observer_ignore = row.observer_ignore;
    }
    db_rows_free(&r);
    if (!ok) {
        free(list);
        return false;
    }
    *out = list;
    *count = n;
    return true;
}

size_t sdb_chat_list(Db *db, const char *project_id, ChatSummary **out)
{
    size_t count = 0;
    (void)sdb_chat_list_checked(db, project_id, out, &count);
    return count;
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
    bool ok = db_rows_has_columns(&r, 8) && db_rows_has_columns(&d, 2);
    int64_t previous_document_seq = -1;
    for (size_t i = 0; ok && i < d.nrows; i++) {
        int64_t seq;
        const char *name;
        ok = chat_document_row(&d, i, &seq, &name) && seq >= previous_document_seq;
        if (ok) previous_document_seq = seq;
    }
    size_t row_count = r.nrows;
    ChatMessage *list = ok && row_count ? calloc(row_count, sizeof(ChatMessage)) : NULL;
    if (row_count && !list) ok = false;
    size_t k2 = 0;
    int64_t previous_message_seq = -1;
    for (size_t i = 0; ok && i < row_count; i++) {
        ChatMessageRow row;
        ok = chat_message_row(&r, i, &row) && row.seq > previous_message_seq &&
             (!strcmp(row.role, "user") || !strcmp(row.role, "assistant"));
        if (!ok) break;
        previous_message_seq = row.seq;
        ChatMessage *m = &list[*n];
        m->role = !strcmp(row.role, "user") ? CHAT_USER : CHAT_ASSISTANT;
        m->content = strdup(row.content);
        m->error = row.error;
        m->is_edit = row.is_edit;
        m->truncated = row.truncated;
        m->via = row.via && row.via[0] ? strdup(row.via) : NULL;
        m->ts = row.created_at;
        ok = m->content && (!row.via || !row.via[0] || m->via);
        if (!ok) break;
        /* The documents' *current* names: renames are followed for free. */
        int64_t seq = row.seq;
        size_t len = 1;
        for (size_t j = k2; ok && j < d.nrows; j++) {
            int64_t document_seq;
            const char *name;
            ok = chat_document_row(&d, j, &document_seq, &name);
            if (!ok || document_seq != seq) break;
            size_t name_len = strlen(name);
            if (name_len > SIZE_MAX - len - 1) ok = false;
            else len += name_len + 1;
        }
        if (!ok) break;
        if (len > 1 && (m->docs = malloc(len)) != NULL) {
            m->docs[0] = '\0';
            for (; k2 < d.nrows; k2++) {
                int64_t document_seq;
                const char *name;
                ok = chat_document_row(&d, k2, &document_seq, &name);
                if (!ok || document_seq != seq) break;
                if (m->docs[0]) strcat(m->docs, "\n");
                strcat(m->docs, name);
            }
        } else if (len > 1) ok = false;
        if (ok && k2 < d.nrows) {
            int64_t document_seq;
            const char *name;
            ok = chat_document_row(&d, k2, &document_seq, &name);
            ok = ok && document_seq > seq;
        }
        if (ok) (*n)++;
    }
    if (ok && k2 != d.nrows) ok = false;
    db_rows_free(&r);
    db_rows_free(&d);
    if (!ok) {
        chat_messages_free_partial(list, row_count);
        *n = 0;
        return false;
    }
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
