#define _DARWIN_C_SOURCE
#include "semantic.h"

#include "core/utf8.h"

#include <CommonCrypto/CommonDigest.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/store.h"
#include "cJSON.h"
#include "llm/http.h"
#include "llm/observer.h"

static LlmServer *g_embed;
static char g_qkey[128];
static char g_qport[24] = "tcp:6333";
static bool g_qloaded;

void semantic_set_embedder(LlmServer *embed) { g_embed = embed; }

static void load_qdrant(void)
{
    if (g_qloaded) return;
    g_qloaded = true;
    const char *port = getenv("SCRIBODO_QDRANT_PORT");
    if (port) snprintf(g_qport, sizeof(g_qport), "tcp:%d", atoi(port));
    char path[1024];
    const char *dir = getenv("SCRIBODO_SECRETS"), *home = getenv("HOME");
    if (dir) snprintf(path, sizeof(path), "%s/qdrant_api_key", dir);
    else snprintf(path, sizeof(path), "%s/Library/Application Support/Scribodo/services/qdrant_api_key", home ? home : "");
    FILE *f = fopen(path, "rb");
    if (f) {
        g_qkey[fread(g_qkey, 1, sizeof(g_qkey) - 1, f)] = '\0';
        g_qkey[strcspn(g_qkey, "\r\n")] = '\0';
        fclose(f);
    }
}

/* One request to Qdrant; *out gets the parsed JSON (NULL on failure). */
static bool qdrant(const char *method, const char *path, cJSON *body, cJSON **out)
{
    load_qdrant();
    if (out) *out = NULL;
    if (!g_qkey[0]) return false;
    char *js = body ? cJSON_PrintUnformatted(body) : NULL;
    HttpResult r;
    bool ok = http_request(g_qport, method, path, g_qkey, js, NULL, NULL, NULL, 15000, &r) && r.status >= 200 &&
              r.status < 300;
    if (!ok && getenv("SCRIBODO_SEM_DEBUG"))
        fprintf(stderr, "qdrant %s %s -> %d %s %.300s\n", method, path, r.status, r.error, r.body ? r.body : "");
    if (ok && out && r.body) *out = cJSON_Parse(r.body);
    http_result_free(&r);
    free(js);
    return ok;
}

/* One collection per project, named after the database too, so a test
 * database's collections can be removed with it and never mix with the
 * author's. */
static void collection_for(const char *pid, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s__%s", store_database(), pid);
}

bool semantic_ready(void)
{
    cJSON *j = NULL;
    bool ok = qdrant("GET", "/collections", NULL, &j);
    cJSON_Delete(j);
    return ok && (!g_embed || llm_server_state(g_embed, NULL, 0) == LLM_READY);
}

/* ---- embeddings ------------------------------------------------------------- */

static void normalise(float *v)
{
    double s = 0;
    for (int i = 0; i < SEM_DIM; i++) s += (double)v[i] * v[i];
    double n = sqrt(s);
    if (n > 0)
        for (int i = 0; i < SEM_DIM; i++) v[i] = (float)(v[i] / n);
}

/* The fake embedder: hashed bag of lowercase words (possessive 's dropped),
 * so texts sharing words are near each other. Deterministic. */
static void fake_embed(const char *text, float *v)
{
    memset(v, 0, SEM_DIM * sizeof(float));
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        while (*p && !(*p >= 0x80 || ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z')) && !(*p >= '0' && *p <= '9')) p++;
        uint64_t h = 1469598103934665603ull;
        size_t n = 0;
        while (*p && (*p >= 0x80 || ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z') || (*p >= '0' && *p <= '9'))) {
            unsigned char c = *p >= 'A' && *p <= 'Z' ? (unsigned char)(*p + 32) : *p;
            h = (h ^ c) * 1099511628211ull;
            p++;
            n++;
        }
        if (*p == '\'' && (p[1] == 's' || p[1] == 'S')) p += 2; /* Zoë's -> zoë */
        if (n >= 2) v[h % SEM_DIM] += (h >> 32) & 1 ? 1.0f : -1.0f;
    }
    normalise(v);
}

static const char *k_query_instruction =
    "Instruct: Given a statement from a work of fiction, retrieve established facts about the same people, places "
    "or things\nQuery: ";

bool semantic_embed(const char *const *texts, size_t n, bool query, float *out, char *err, size_t errlen)
{
    if (!g_embed) {
        for (size_t i = 0; i < n; i++) fake_embed(texts[i], out + i * SEM_DIM);
        return true;
    }
    if (llm_server_state(g_embed, NULL, 0) != LLM_READY) {
        snprintf(err, errlen, "The embedding model isn't ready");
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "input");
    for (size_t i = 0; i < n; i++) {
        if (query) {
            size_t len = strlen(k_query_instruction) + strlen(texts[i]) + 1;
            char *q = malloc(len);
            if (!q) {
                cJSON_Delete(root);
                return false;
            }
            snprintf(q, len, "%s%s", k_query_instruction, texts[i]);
            cJSON_AddItemToArray(arr, cJSON_CreateString(q));
            free(q);
        } else {
            cJSON_AddItemToArray(arr, cJSON_CreateString(texts[i]));
        }
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    HttpResult r;
    bool ok = body && http_request(llm_server_socket(g_embed), "POST", "/v1/embeddings", llm_server_key(g_embed), body, NULL,
                                   NULL, NULL, 120000, &r) &&
              r.status == 200 && r.body;
    free(body);
    cJSON *j = ok ? cJSON_Parse(r.body) : NULL;
    if (!ok) snprintf(err, errlen, "Embedding request failed (%d %s)", r.status, r.error);
    http_result_free(&r);
    cJSON *data = cJSON_GetObjectItem(j, "data"), *item;
    size_t got = 0;
    cJSON_ArrayForEach(item, data)
    {
        cJSON *idx = cJSON_GetObjectItem(item, "index"), *emb = cJSON_GetObjectItem(item, "embedding");
        size_t i = cJSON_IsNumber(idx) ? (size_t)idx->valuedouble : got;
        if (i >= n || cJSON_GetArraySize(emb) != SEM_DIM) continue;
        float *v = out + i * SEM_DIM;
        int d = 0;
        cJSON *x;
        cJSON_ArrayForEach(x, emb) v[d++] = (float)x->valuedouble;
        normalise(v);
        got++;
    }
    cJSON_Delete(j);
    if (ok && got != n) {
        snprintf(err, errlen, "The embedding model returned %zu of %zu vectors", got, n);
        ok = false;
    }
    return ok;
}

/* ---- the index ---------------------------------------------------------------- */

/* A stable UUID for a point key (Qdrant ids are integers or UUIDs; a large
 * integer can't pass through cJSON's doubles exactly, so UUIDs it is). */
static void point_id(const char *key, char out[37])
{
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(key, (CC_LONG)strlen(key), d);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2], d[3], d[4],
             d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
}

static void hash16(const char *s, char out[17])
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = (h ^ *p) * 1099511628211ull;
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* *created: it didn't exist (a new project, or a lost index). */
static bool ensure_collection_ex(const char *pid, bool *created)
{
    char name[160], path[220];
    collection_for(pid, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s", name);
    if (created) *created = false;
    cJSON *j = NULL;
    if (qdrant("GET", path, NULL, &j)) {
        cJSON_Delete(j);
        return true;
    }
    if (created) *created = true;
    cJSON *body = cJSON_CreateObject();
    cJSON *vec = cJSON_AddObjectToObject(body, "vectors");
    cJSON_AddNumberToObject(vec, "size", SEM_DIM);
    cJSON_AddStringToObject(vec, "distance", "Cosine");
    bool ok = qdrant("PUT", path, body, NULL);
    cJSON_Delete(body);
    return ok;
}

static bool ensure_collection(const char *pid) { return ensure_collection_ex(pid, NULL); }

/* Has this exact text already been indexed under `key`? */
static bool marked(Db *db, const char *pid, const char *key, const char *hash)
{
    DbRows r;
    if (!db_query(db, "SELECT content_hash FROM semantic_marks WHERE project_id = ? AND point_key = ?",
                  (DbArg[2]){ DB_S(pid), DB_S(key) }, 2, &r))
        return false;
    bool same = r.nrows && db_cell(&r, 0, 0) && !strcmp(db_cell(&r, 0, 0), hash);
    db_rows_free(&r);
    return same;
}

static void mark(Db *db, const char *pid, const char *key, const char *hash)
{
    db_run(db,
           "INSERT INTO semantic_marks (project_id, point_key, content_hash) VALUES (?,?,?)"
           " ON DUPLICATE KEY UPDATE content_hash = VALUES(content_hash)",
           (DbArg[3]){ DB_S(pid), DB_S(key), DB_S(hash) }, 3, NULL);
}

typedef struct {
    char key[191];
    char hash[17];  /* what the mark records (text, or status + text) */
    const char *text;
    cJSON *payload; /* owned until upsert */
} Pending;

/* Embed and upsert a batch; marks each on success. */
static bool upsert(Db *db, const char *pid, Pending *p, size_t n)
{
    if (n == 0) return true;
    float *vecs = malloc(n * SEM_DIM * sizeof(float));
    const char **texts = malloc(n * sizeof(char *));
    bool ok = vecs && texts;
    for (size_t i = 0; ok && i < n; i++) texts[i] = p[i].text;
    char err[200];
    ok = ok && semantic_embed(texts, n, false, vecs, err, sizeof(err));
    cJSON *body = cJSON_CreateObject();
    cJSON *pts = cJSON_AddArrayToObject(body, "points");
    for (size_t i = 0; ok && i < n; i++) {
        cJSON *pt = cJSON_CreateObject();
        char uuid[37];
        point_id(p[i].key, uuid);
        cJSON_AddStringToObject(pt, "id", uuid);
        cJSON *v = cJSON_AddArrayToObject(pt, "vector");
        for (int d = 0; d < SEM_DIM; d++) cJSON_AddItemToArray(v, cJSON_CreateNumber(vecs[i * SEM_DIM + d]));
        cJSON_AddItemToObject(pt, "payload", p[i].payload);
        p[i].payload = NULL;
        cJSON_AddItemToArray(pts, pt);
    }
    char name[160], path[260];
    collection_for(pid, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s/points?wait=true", name);
    ok = ok && qdrant("PUT", path, body, NULL);
    cJSON_Delete(body);
    for (size_t i = 0; i < n; i++) {
        if (ok) mark(db, pid, p[i].key, p[i].hash);
        cJSON_Delete(p[i].payload);
        p[i].payload = NULL;
    }
    free(vecs);
    free(texts);
    return ok;
}

static cJSON *payload_base(const char *pid, const char *kind, const char *text)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "project_id", pid);
    cJSON_AddStringToObject(p, "kind", kind);
    cJSON_AddStringToObject(p, "text", text);
    return p;
}

static bool delete_points(const char *pid, const char *key, const char *value)
{
    char name[160], path[260];
    collection_for(pid, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s/points/delete?wait=true", name);
    cJSON *body = cJSON_CreateObject();
    cJSON *must = cJSON_AddArrayToObject(cJSON_AddObjectToObject(body, "filter"), "must");
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "key", key);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(c, "match"), "value", value);
    cJSON_AddItemToArray(must, c);
    bool ok = qdrant("POST", path, body, NULL);
    cJSON_Delete(body);
    return ok;
}

static bool sync_records_impl(const char *project_id, char *err, size_t errlen);

static bool rebuild_pending(Db *db, const char *pid)
{
    DbRows r;
    if (!db_query(db, "SELECT index_rebuild_pending FROM projects WHERE id = ?", (DbArg[1]){ DB_S(pid) }, 1, &r)) return false;
    bool p = r.nrows && db_cell(&r, 0, 0) && atoi(db_cell(&r, 0, 0)) == 1;
    db_rows_free(&r);
    return p;
}

static void set_rebuild_pending(Db *db, const char *pid, bool on)
{
    db_run(db, "UPDATE projects SET index_rebuild_pending = ? WHERE id = ?", (DbArg[2]){ DB_I(on ? 1 : 0), DB_S(pid) }, 2, NULL);
}

bool semantic_sync_records(const char *project_id, char *err, size_t errlen)
{
    Db *db = store_db();
    bool created = false;
    if (!db || !ensure_collection_ex(project_id, &created)) {
        snprintf(err, errlen, "The semantic index isn't available");
        return false;
    }
    /* No collection (a new project, or an index that was lost: restored
     * from a backup, Qdrant wiped), or a rebuild that didn't finish: the
     * marks can't be trusted, so the whole index is rebuilt from the
     * store (red-team 009). */
    if (errlen) err[0] = '\0';
    if (created || rebuild_pending(db, project_id)) return semantic_rebuild(project_id, err, errlen) >= 0;
    return sync_records_impl(project_id, err, errlen);
}

static bool sync_records_impl(const char *project_id, char *err, size_t errlen)
{
    Db *db = store_db();
    if (!db || !ensure_collection(project_id)) {
        snprintf(err, errlen, "The semantic index isn't available");
        return false;
    }
    DbRows r;
    /* The current version of every note and canon entry. */
    if (!db_query(db,
                  "SELECT r.id, r.kind, v.status, v.content FROM records r JOIN record_versions v ON v.record_id = r.id"
                  " WHERE r.project_id = ? AND r.kind IN ('note','canon')"
                  " AND v.ver = (SELECT MAX(ver) FROM record_versions WHERE record_id = r.id)",
                  (DbArg[1]){ DB_S(project_id) }, 1, &r)) {
        snprintf(err, errlen, "%s", db_error(db));
        return false;
    }
    Pending *p = calloc(r.nrows ? r.nrows : 1, sizeof(Pending));
    size_t np = 0;
    bool ok = p != NULL;
    for (size_t i = 0; ok && i < r.nrows; i++) {
        const char *id = db_cell(&r, i, 0), *kind = db_cell(&r, i, 1), *status = db_cell(&r, i, 2), *text = db_cell(&r, i, 3);
        char key[191], h[17], tag[600];
        snprintf(key, sizeof(key), "rec:%s", id);
        /* The status is part of what's indexed: a retracted entry must stop
         * counting as canon. */
        snprintf(tag, sizeof(tag), "%s|%.500s", status, text);
        hash16(tag, h);
        if (!strcmp(status, "deleted")) {
            if (!marked(db, project_id, key, "deleted0000000000")) {
                delete_points(project_id, "record_id", id);
                mark(db, project_id, key, "deleted0000000000");
            }
            continue;
        }
        if (marked(db, project_id, key, h)) continue;
        Pending *q = &p[np++];
        snprintf(q->key, sizeof(q->key), "%s", key);
        snprintf(q->hash, sizeof(q->hash), "%s", h);
        q->text = text;
        q->payload = payload_base(project_id, !strcmp(kind, "canon") ? "canon" : "note", text);
        cJSON_AddStringToObject(q->payload, "record_id", id);
        cJSON_AddStringToObject(q->payload, "status", status);
    }
    for (size_t i = 0; ok && i < np; i += 16) {
        size_t n = np - i < 16 ? np - i : 16;
        ok = upsert(db, project_id, p + i, n);
    }
    for (size_t i = 0; i < np; i++) cJSON_Delete(p[i].payload);
    free(p);
    db_rows_free(&r);
    if (!ok) snprintf(err, errlen, "Indexing notes and canon failed");
    return ok;
}

bool semantic_index_text(const char *project_id, const char *kind, const char *document_id, const char *revision_id,
                         const char *conversation_id, long seq, size_t start, size_t len, const char *text)
{
    Db *db = store_db();
    if (!db || !text || !text[0] || !ensure_collection(project_id)) return false;
    char key[191], h[17];
    hash16(text, h);
    if (!strcmp(kind, "draft")) snprintf(key, sizeof(key), "doc:%s:%s", document_id ? document_id : "", h);
    else snprintf(key, sizeof(key), "chat:%s:%ld:%zu", conversation_id ? conversation_id : "", seq, start);
    if (marked(db, project_id, key, h)) return true;
    Pending p = { .text = text };
    snprintf(p.key, sizeof(p.key), "%s", key);
    snprintf(p.hash, sizeof(p.hash), "%s", h);
    p.payload = payload_base(project_id, kind, text);
    if (document_id) cJSON_AddStringToObject(p.payload, "document_id", document_id);
    if (revision_id) cJSON_AddStringToObject(p.payload, "revision_id", revision_id);
    if (conversation_id) cJSON_AddStringToObject(p.payload, "conversation_id", conversation_id);
    cJSON_AddStringToObject(p.payload, "piece_hash", h);
    cJSON_AddNumberToObject(p.payload, "seq", (double)seq);
    cJSON_AddNumberToObject(p.payload, "start", (double)start);
    cJSON_AddNumberToObject(p.payload, "len", (double)len);
    return upsert(db, project_id, &p, 1);
}

bool semantic_prune_doc(const char *project_id, const char *document_id, const char *const *piece_hashes, size_t n)
{
    Db *db = store_db();
    if (!db) return false;
    char like[120];
    snprintf(like, sizeof(like), "doc:%s:%%", document_id);
    DbRows r;
    if (!db_query(db, "SELECT point_key FROM semantic_marks WHERE project_id = ? AND point_key LIKE ?",
                  (DbArg[2]){ DB_S(project_id), DB_S(like) }, 2, &r))
        return false;
    bool ok = true;
    size_t prefix = strlen(like) - 1;
    for (size_t i = 0; i < r.nrows; i++) {
        const char *key = db_cell(&r, i, 0), *h = key + prefix;
        bool current = false;
        for (size_t k = 0; k < n && !current; k++) current = !strcmp(piece_hashes[k], h);
        if (current) continue;
        /* This piece is gone from the document: its point and mark go too. */
        char name[160], path[260];
        collection_for(project_id, name, sizeof(name));
        snprintf(path, sizeof(path), "/collections/%s/points/delete?wait=true", name);
        cJSON *body = cJSON_CreateObject();
        cJSON *must = cJSON_AddArrayToObject(cJSON_AddObjectToObject(body, "filter"), "must");
        cJSON *a = cJSON_CreateObject(), *b = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "key", "document_id");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(a, "match"), "value", document_id);
        cJSON_AddStringToObject(b, "key", "piece_hash");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(b, "match"), "value", h);
        cJSON_AddItemToArray(must, a);
        cJSON_AddItemToArray(must, b);
        ok = qdrant("POST", path, body, NULL) && ok;
        cJSON_Delete(body);
        db_run(db, "DELETE FROM semantic_marks WHERE project_id = ? AND point_key = ?", (DbArg[2]){ DB_S(project_id), DB_S(key) },
               2, NULL);
    }
    db_rows_free(&r);
    return ok;
}

void semantic_hits_free(SemHit *h, size_t n)
{
    for (size_t i = 0; i < n; i++) free(h[i].text);
    free(h);
}

/* A payload number, 0 if missing or out of range (a cast of NaN or of a
 * huge value is undefined). */
static double payload_num(const cJSON *pl, const char *name)
{
    const cJSON *x = cJSON_GetObjectItem(pl, name);
    if (!cJSON_IsNumber(x)) return 0;
    double v = x->valuedouble;
    return v >= 0 && v <= 9007199254740992.0 ? v : 0;
}

static size_t search_filtered(const char *pid, const char *query, cJSON *filter, size_t limit, SemHit **out)
{
    *out = NULL;
    float *v = malloc(SEM_DIM * sizeof(float));
    char err[200];
    if (!v || !semantic_embed(&query, 1, true, v, err, sizeof(err))) {
        free(v);
        cJSON_Delete(filter);
        return 0;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON *vec = cJSON_AddArrayToObject(body, "vector");
    for (int d = 0; d < SEM_DIM; d++) cJSON_AddItemToArray(vec, cJSON_CreateNumber(v[d]));
    free(v);
    cJSON_AddNumberToObject(body, "limit", (double)limit);
    cJSON_AddBoolToObject(body, "with_payload", true);
    if (filter) cJSON_AddItemToObject(body, "filter", filter);
    char name[160], path[260];
    collection_for(pid, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s/points/search", name);
    cJSON *res = NULL;
    bool ok = qdrant("POST", path, body, &res);
    cJSON_Delete(body);
    cJSON *arr = ok ? cJSON_GetObjectItem(res, "result") : NULL;
    size_t n = (size_t)cJSON_GetArraySize(arr), k = 0;
    SemHit *hits = n ? calloc(n, sizeof(SemHit)) : NULL;
    cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        if (!hits) break;
        cJSON *pl = cJSON_GetObjectItem(it, "payload");
        SemHit *h = &hits[k];
        h->score = cJSON_GetNumberValue(cJSON_GetObjectItem(it, "score"));
#define S(field, name)                                                                     \
    do {                                                                                   \
        cJSON *x = cJSON_GetObjectItem(pl, name);                                          \
        if (cJSON_IsString(x)) snprintf(h->field, sizeof(h->field), "%s", x->valuestring); \
    } while (0)
        S(kind, "kind");
        S(record_id, "record_id");
        S(document_id, "document_id");
        S(revision_id, "revision_id");
        S(conversation_id, "conversation_id");
        S(status, "status");
#undef S
        cJSON *t = cJSON_GetObjectItem(pl, "text");
        h->text = strdup(cJSON_IsString(t) ? t->valuestring : "");
        h->seq = (long)payload_num(pl, "seq");
        h->start = (size_t)payload_num(pl, "start");
        h->len = (size_t)payload_num(pl, "len");
        /* Payloads come from our own writes, but check the project anyway. */
        cJSON *pp = cJSON_GetObjectItem(pl, "project_id");
        if (h->text && cJSON_IsString(pp) && !strcmp(pp->valuestring, pid)) k++;
        else free(h->text);
    }
    cJSON_Delete(res);
    *out = hits;
    return k;
}

static cJSON *match(const char *key, const char *value)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "key", key);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(c, "match"), "value", value);
    return c;
}

size_t semantic_canon_for(const char *project_id, const char *statement, size_t limit, SemHit **out)
{
    /* Only committed canon: retracted entries and everything else (draft
     * text, notes, chat) are never treated as established fact. */
    cJSON *filter = cJSON_CreateObject();
    cJSON *must = cJSON_AddArrayToObject(filter, "must");
    cJSON_AddItemToArray(must, match("kind", "canon"));
    cJSON_AddItemToArray(must, match("status", "committed"));
    return search_filtered(project_id, statement, filter, limit, out);
}

size_t semantic_search(const char *project_id, const char *query, const char *kind, size_t limit, SemHit **out)
{
    cJSON *filter = NULL;
    if (kind) {
        filter = cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_AddArrayToObject(filter, "must"), match("kind", kind));
    }
    return search_filtered(project_id, query, filter, limit, out);
}

bool semantic_drop(const char *project_id)
{
    char name[160], path[220];
    collection_for(project_id, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s", name);
    bool ok = qdrant("DELETE", path, NULL, NULL);
    Db *db = store_db();
    if (db) db_run(db, "DELETE FROM semantic_marks WHERE project_id = ?", (DbArg[1]){ DB_S(project_id) }, 1, NULL);
    return ok;
}

long semantic_rebuild(const char *project_id, char *err, size_t errlen)
{
    Db *db = store_db();
    if (!db) {
        snprintf(err, errlen, "The project store is offline");
        return -1;
    }
    /* Pending until every part is indexed: an interrupted rebuild (an
     * embedding error, the app quitting) is resumed by the next sync. */
    set_rebuild_pending(db, project_id, true);
    semantic_drop(project_id); /* fine if it didn't exist */
    if (!ensure_collection(project_id)) {
        snprintf(err, errlen, "Qdrant isn't reachable");
        return -1;
    }
    if (!sync_records_impl(project_id, err, errlen)) return -1;
    long count = 0, failed = 0;
    DbRows r;
    /* Each document's current revision, in paragraphs. */
    if (db_query(db,
                 "SELECT d.id, v.id, v.content FROM documents d JOIN revisions v ON v.id = d.current_revision_id"
                 " WHERE d.project_id = ?",
                 (DbArg[1]){ DB_S(project_id) }, 1, &r)) {
        for (size_t i = 0; i < r.nrows; i++) {
            const char *text = db_cell(&r, i, 2);
            size_t cap = 256, np = 0;
            size_t *ps = malloc(cap * sizeof(size_t)), *pl = malloc(cap * sizeof(size_t));
            while (ps && pl && (np = obs_paragraphs(text, ps, pl, cap)) == cap) { /* every paragraph */
                cap *= 2;
                size_t *a = realloc(ps, cap * sizeof(size_t)), *b = realloc(pl, cap * sizeof(size_t));
                if (!a || !b) {
                    free(a ? a : ps);
                    free(b ? b : pl);
                    ps = pl = NULL;
                    np = 0;
                    break;
                }
                ps = a;
                pl = b;
            }
            for (size_t k = 0; k < np; k++) {
                char *para = malloc(pl[k] + 1), *piece = malloc(2001);
                if (!para || !piece) {
                    free(para);
                    free(piece);
                    failed++;
                    continue;
                }
                memcpy(para, text + ps[k], pl[k]);
                para[pl[k]] = '\0';
                size_t len = utf8_clip(piece, 2001, para, 2000); /* whole characters */
                free(para);
                size_t cps = 0;
                for (size_t b = 0; b < ps[k]; b++) cps += ((unsigned char)text[b] & 0xC0) != 0x80;
                size_t clen = 0;
                for (size_t b = 0; b < len; b++) clen += ((unsigned char)piece[b] & 0xC0) != 0x80;
                if (semantic_index_text(project_id, "draft", db_cell(&r, i, 0), db_cell(&r, i, 1), NULL, 0, cps, clen, piece)) count++;
                else failed++;
                free(piece);
            }
            free(ps);
            free(pl);
        }
        db_rows_free(&r);
    }
    /* Chat messages (not failed replies or AI rewrites). */
    if (db_query(db,
                 "SELECT m.conversation_id, m.seq, m.content FROM messages m JOIN conversations c ON c.id = m.conversation_id"
                 " WHERE c.project_id = ? AND m.error = 0 AND NOT (m.role = 'assistant' AND m.is_edit = 1)",
                 (DbArg[1]){ DB_S(project_id) }, 1, &r)) {
        for (size_t i = 0; i < r.nrows; i++) {
            const char *text = db_cell(&r, i, 2) ? db_cell(&r, i, 2) : "";
            char *piece = malloc(2001);
            if (!piece) {
                failed++;
                continue;
            }
            utf8_clip(piece, 2001, text, 2000);
            size_t clen = 0; /* the range is in characters */
            for (const char *b = piece; *b; b++) clen += ((unsigned char)*b & 0xC0) != 0x80;
            if (semantic_index_text(project_id, "chat", NULL, NULL, db_cell(&r, i, 0), atol(db_cell(&r, i, 1)), 0, clen, piece))
                count++;
            else failed++;
            free(piece);
        }
        db_rows_free(&r);
    }
    DbRows rc;
    if (db_query(db, "SELECT COUNT(*) FROM records WHERE project_id = ? AND kind IN ('note','canon')",
                 (DbArg[1]){ DB_S(project_id) }, 1, &rc)) {
        count += atol(db_cell(&rc, 0, 0));
        db_rows_free(&rc);
    }
    if (failed) {
        snprintf(err, errlen, "%ld passages couldn't be indexed; the rebuild resumes later", failed);
        return -1;
    }
    set_rebuild_pending(db, project_id, false);
    return count;
}

bool semantic_forget_collection(const char *project_id)
{
    char name[160], path[220];
    collection_for(project_id, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s", name);
    return qdrant("DELETE", path, NULL, NULL);
}
