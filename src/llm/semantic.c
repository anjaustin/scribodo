#define _DARWIN_C_SOURCE
#include "semantic.h"
#include "core/experiments.h"
#include "memory_vectors.h"
#include "app/memory_index_text.h"

#include "core/utf8.h"

#include <CommonCrypto/CommonDigest.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/store.h"
#include "core/json.h"
#include "llm/http.h"
#include "llm/observer.h"

struct SemanticClient {
    StoreManager *store;
    LlmServer *embed;
    char qkey[128];
    char qport[24];
    atomic_bool *cancel;
    double deadline_ms;
};

SemanticClient *semantic_client_bounded(SemanticClient *client, atomic_bool *cancel, double deadline_ms)
{
    if (!client || !isfinite(deadline_ms) || deadline_ms <= 0) return NULL;
    SemanticClient *copy = malloc(sizeof(*copy));
    if (copy) { *copy = *client; copy->cancel = cancel; copy->deadline_ms = deadline_ms; }
    return copy;
}

static bool semantic_http(SemanticClient *client, const char *socket, const char *method, const char *path,
                           const char *key, const char *body, int timeout_ms, HttpResult *result)
{
    if (client->deadline_ms > 0)
        return http_request_until(socket, method, path, key, body, NULL, NULL, client->cancel,
                                  timeout_ms, client->deadline_ms, result);
    return http_request(socket, method, path, key, body, NULL, NULL, NULL, timeout_ms, result);
}

static bool semantic_database(SemanticClient *client, char *out, size_t size)
{
    if (!client || !out || !size) return false;
    if (client->deadline_ms > 0)
        return store_database_until(client->store, out, size, client->cancel, client->deadline_ms);
    store_database(client->store, out, size);
    return out[0] != 0;
}

SemanticClient *semantic_client_create(StoreManager *store, LlmServer *embed)
{
    if (!store) return NULL;
    SemanticClient *client = calloc(1, sizeof(*client));
    if (!client) return NULL;
    client->store = store;
    client->embed = embed;
    snprintf(client->qport, sizeof(client->qport), "tcp:6333");
    const char *port = getenv("SCRIBODO_QDRANT_PORT");
    if (port) snprintf(client->qport, sizeof(client->qport), "tcp:%d", atoi(port));
    char path[1024];
    const char *dir = getenv("SCRIBODO_SECRETS"), *home = getenv("HOME");
    if (dir) snprintf(path, sizeof(path), "%s/qdrant_api_key", dir);
    else snprintf(path, sizeof(path), "%s/Library/Application Support/Scribodo/services/qdrant_api_key", home ? home : "");
    FILE *f = fopen(path, "rb");
    if (f) {
        client->qkey[fread(client->qkey, 1, sizeof(client->qkey) - 1, f)] = '\0';
        client->qkey[strcspn(client->qkey, "\r\n")] = '\0';
        fclose(f);
    }
    return client;
}

void semantic_client_destroy(SemanticClient *client) { free(client); }

/* One request to Qdrant; *out gets the parsed JSON (NULL on failure). */
static bool qdrant(SemanticClient *client, const char *method, const char *path, cJSON *body, cJSON **out)
{
    if (out) *out = NULL;
    if (!client || !client->qkey[0]) return false;
    char *js = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body && !js) return false;
    HttpResult r;
    bool ok = semantic_http(client, client->qport, method, path, client->qkey, js, 15000, &r) && r.status >= 200 &&
              r.status < 300;
    if (!ok && experiments()->semantic_debug)
        fprintf(stderr, "qdrant %s %s -> %d %s %.300s\n", method, path, r.status, r.error, r.body ? r.body : "");
    if (ok && out && r.body) *out = json_parse(r.body);
    http_result_free(&r);
    free(js);
    return ok;
}

/* One collection per project, named after the database too, so a test
 * database's collections can be removed with it and never mix with the
 * author's. */
static void collection_for(SemanticClient *client, const char *pid, char *out, size_t outlen)
{
    char database[64];
    store_database(client->store, database, sizeof(database));
    snprintf(out, outlen, "%s__%s", database, pid);
}

bool semantic_ready(SemanticClient *client)
{
    cJSON *j = NULL;
    bool ok = qdrant(client, "GET", "/collections", NULL, &j);
    cJSON_Delete(j);
    return ok && semantic_embedding_ready(client);
}

bool semantic_embedding_ready(SemanticClient *client)
{
    return client && (!client->embed || llm_server_state(client->embed, NULL, 0) == LLM_READY);
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

bool semantic_embed(SemanticClient *client, const char *const *texts, size_t n, bool query, float *out, char *err, size_t errlen)
{
    if (errlen) err[0] = '\0';
    if (!n) return true;
    if (n > SIZE_MAX / SEM_DIM / sizeof(float)) {
        snprintf(err, errlen, "Too many embedding inputs");
        return false;
    }
    if (!client || !client->embed) {
        for (size_t i = 0; i < n; i++) fake_embed(texts[i], out + i * SEM_DIM);
        return true;
    }
    if (llm_server_state(client->embed, NULL, 0) != LLM_READY) {
        snprintf(err, errlen, "The embedding model isn't ready");
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = root ? cJSON_AddArrayToObject(root, "input") : NULL;
    if (!arr) {
        cJSON_Delete(root);
        snprintf(err, errlen, "Not enough memory to build the embedding request");
        return false;
    }
    /* Every input within the embedding server's context (its size is set
     * to fit this): long notes or canon entries are embedded by their
     * first SEM_MAX_INPUT_BYTES, on a character boundary. */
    char *clip = malloc(SEM_MAX_INPUT_BYTES + 1);
    if (!clip) {
        cJSON_Delete(root);
        snprintf(err, errlen, "Not enough memory to build the embedding request");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        cJSON *item = NULL;
        utf8_clip(clip, SEM_MAX_INPUT_BYTES + 1, texts[i] ? texts[i] : "", SEM_MAX_INPUT_BYTES);
        if (query) {
            size_t len = strlen(k_query_instruction) + strlen(clip) + 1;
            char *q = malloc(len);
            if (!q) {
                free(clip);
                cJSON_Delete(root);
                snprintf(err, errlen, "Not enough memory to build the embedding request");
                return false;
            }
            snprintf(q, len, "%s%s", k_query_instruction, clip);
            item = cJSON_CreateString(q);
            free(q);
        } else {
            item = cJSON_CreateString(clip);
        }
        if (!item || !cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item);
            free(clip);
            cJSON_Delete(root);
            snprintf(err, errlen, "Not enough memory to build the embedding request");
            return false;
        }
    }
    free(clip);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        snprintf(err, errlen, "Not enough memory to serialize the embedding request");
        return false;
    }
    HttpResult r = { 0 };
    bool ok = semantic_http(client, llm_server_socket(client->embed), "POST", "/v1/embeddings", llm_server_key(client->embed), body,
                            120000, &r) &&
              r.status == 200 && r.body;
    free(body);
    cJSON *j = ok ? json_parse_complete(r.body, r.len) : NULL;
    if (!ok) snprintf(err, errlen, "Embedding request failed (%d %s)", r.status, r.error);
    http_result_free(&r);
    cJSON *data = cJSON_GetObjectItem(j, "data"), *item;
    /* Validate the complete response before publishing any output. A repeated
     * index must not hide a missing vector, nor may malformed numbers reach an
     * integer cast or normalization of uninitialized data. */
    bool *seen = ok ? calloc(n, sizeof(*seen)) : NULL;
    ok = ok && seen && cJSON_IsArray(data) && (size_t)cJSON_GetArraySize(data) == n;
    size_t seq = 0;
    cJSON_ArrayForEach(item, data)
    {
        if (!ok) break;
        cJSON *idx = cJSON_GetObjectItem(item, "index"), *emb = cJSON_GetObjectItem(item, "embedding");
        double index = idx ? (cJSON_IsNumber(idx) ? idx->valuedouble : -1) : (double)seq;
        ok = cJSON_IsObject(item) && isfinite(index) && index >= 0 && index < (double)n && floor(index) == index &&
             cJSON_IsArray(emb) && cJSON_GetArraySize(emb) == SEM_DIM;
        if (!ok) break;
        size_t i = (size_t)index;
        if (seen[i]) { ok = false; break; }
        seen[i] = true;
        cJSON *x;
        cJSON_ArrayForEach(x, emb) {
            if (!cJSON_IsNumber(x) || !isfinite(x->valuedouble) || fabs(x->valuedouble) > FLT_MAX) {
                ok = false;
                break;
            }
        }
        seq++;
    }
    if (ok) {
        seq = 0;
        cJSON_ArrayForEach(item, data) {
            cJSON *idx = cJSON_GetObjectItem(item, "index"), *emb = cJSON_GetObjectItem(item, "embedding"), *x;
            size_t i = idx ? (size_t)idx->valuedouble : seq;
            float *v = out + i * SEM_DIM;
            int d = 0;
            cJSON_ArrayForEach(x, emb) v[d++] = (float)x->valuedouble;
            normalise(v);
            seq++;
        }
    } else if (errlen && !err[0]) {
        snprintf(err, errlen, "The embedding response is incomplete, invalid, or couldn't be allocated");
    }
    free(seen);
    cJSON_Delete(j);
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
static bool ensure_collection_ex(SemanticClient *client, const char *pid, bool *created)
{
    char name[160], path[220];
    collection_for(client, pid, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s", name);
    if (created) *created = false;
    cJSON *j = NULL;
    if (qdrant(client, "GET", path, NULL, &j)) {
        cJSON_Delete(j);
        return true;
    }
    if (created) *created = true;
    cJSON *body = cJSON_CreateObject();
    cJSON *vec = cJSON_AddObjectToObject(body, "vectors");
    cJSON_AddNumberToObject(vec, "size", SEM_DIM);
    cJSON_AddStringToObject(vec, "distance", "Cosine");
    bool ok = qdrant(client, "PUT", path, body, NULL);
    cJSON_Delete(body);
    return ok;
}

static bool ensure_collection(SemanticClient *client, const char *pid) { return ensure_collection_ex(client, pid, NULL); }

/* Has this exact text already been indexed under `key`? */
static bool marked(Db *db, const char *pid, const char *key, const char *hash)
{
    DbRows r;
    if (!db_query(db, "SELECT content_hash FROM semantic_marks WHERE project_id = ? AND point_key = ?",
                  (DbArg[2]){ DB_S(pid), DB_S(key) }, 2, &r))
        return false;
    const char *stored_hash;
    bool same = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_text(&r, 0, 0, &stored_hash) &&
                !strcmp(stored_hash, hash);
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
static bool upsert(SemanticClient *client, Db *db, const char *pid, Pending *p, size_t n)
{
    if (n == 0) return true;
    float *vecs = malloc(n * SEM_DIM * sizeof(float));
    const char **texts = malloc(n * sizeof(char *));
    bool ok = vecs && texts;
    for (size_t i = 0; ok && i < n; i++) texts[i] = p[i].text;
    char err[200];
    ok = ok && semantic_embed(client, texts, n, false, vecs, err, sizeof(err));
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
    collection_for(client, pid, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s/points?wait=true", name);
    ok = ok && qdrant(client, "PUT", path, body, NULL);
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

static bool delete_points(SemanticClient *client, const char *pid, const char *key, const char *value)
{
    char name[160], path[260];
    collection_for(client, pid, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s/points/delete?wait=true", name);
    cJSON *body = cJSON_CreateObject();
    cJSON *must = cJSON_AddArrayToObject(cJSON_AddObjectToObject(body, "filter"), "must");
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "key", key);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(c, "match"), "value", value);
    cJSON_AddItemToArray(must, c);
    bool ok = qdrant(client, "POST", path, body, NULL);
    cJSON_Delete(body);
    return ok;
}

static bool sync_records_impl(SemanticClient *client, const char *project_id, char *err, size_t errlen);

static bool rebuild_pending(Db *db, const char *pid)
{
    DbRows r;
    if (!db_query(db, "SELECT index_rebuild_pending FROM projects WHERE id = ?", (DbArg[1]){ DB_S(pid) }, 1, &r)) return false;
    int64_t value;
    bool p = r.nrows == 1 && db_rows_has_columns(&r, 1) && db_cell_i64(&r, 0, 0, &value) && value == 1;
    db_rows_free(&r);
    return p;
}

static void set_rebuild_pending(Db *db, const char *pid, bool on)
{
    db_run(db, "UPDATE projects SET index_rebuild_pending = ? WHERE id = ?", (DbArg[2]){ DB_I(on ? 1 : 0), DB_S(pid) }, 2, NULL);
}

bool semantic_sync_records(SemanticClient *client, const char *project_id, char *err, size_t errlen)
{
    Db *db = store_db(client->store);
    bool created = false;
    if (!db || !ensure_collection_ex(client, project_id, &created)) {
        snprintf(err, errlen, "The semantic index isn't available");
        return false;
    }
    /* No collection (a new project, or an index that was lost: restored
     * from a backup, Qdrant wiped), or a rebuild that didn't finish: the
     * marks can't be trusted, so the whole index is rebuilt from the
     * store (red-team 009). */
    if (errlen) err[0] = '\0';
    if (created || rebuild_pending(db, project_id)) return semantic_rebuild(client, project_id, err, errlen) >= 0;
    return sync_records_impl(client, project_id, err, errlen);
}

static bool sync_records_impl(SemanticClient *client, const char *project_id, char *err, size_t errlen)
{
    Db *db = store_db(client->store);
    if (!db || !ensure_collection(client, project_id)) {
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
    bool ok = p != NULL && db_rows_has_columns(&r, 4);
    for (size_t i = 0; ok && i < r.nrows; i++) {
        const char *id, *kind, *status, *text;
        ok = db_cell_text(&r, i, 0, &id) && db_cell_text(&r, i, 1, &kind) &&
             db_cell_text(&r, i, 2, &status) && db_cell_text(&r, i, 3, &text) && strlen(id) < 180 &&
             (!strcmp(kind, "note") || !strcmp(kind, "canon"));
        if (!ok) break;
        char key[191], h[17], tag[600];
        snprintf(key, sizeof(key), "rec:%s", id);
        /* The status is part of what's indexed: a retracted entry must stop
         * counting as canon. */
        snprintf(tag, sizeof(tag), "%s|%.500s", status, text);
        hash16(tag, h);
        if (!strcmp(status, "deleted")) {
            if (!marked(db, project_id, key, "deleted0000000000")) {
                delete_points(client, project_id, "record_id", id);
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
        ok = upsert(client, db, project_id, p + i, n);
    }
    for (size_t i = 0; i < np; i++) cJSON_Delete(p[i].payload);
    free(p);
    db_rows_free(&r);
    if (!ok) snprintf(err, errlen, "Indexing notes and canon failed");
    return ok;
}

bool semantic_index_text(SemanticClient *client, const char *project_id, const char *kind, const char *document_id, const char *revision_id,
                         const char *conversation_id, long seq, size_t start, size_t len, const char *text)
{
    Db *db = store_db(client->store);
    if (!db || !text || !text[0] || !ensure_collection(client, project_id)) return false;
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
    return upsert(client, db, project_id, &p, 1);
}

bool semantic_prune_doc(SemanticClient *client, const char *project_id, const char *document_id, const char *const *piece_hashes, size_t n)
{
    Db *db = store_db(client->store);
    if (!db) return false;
    char like[120];
    snprintf(like, sizeof(like), "doc:%s:%%", document_id);
    DbRows r;
    if (!db_query(db, "SELECT point_key FROM semantic_marks WHERE project_id = ? AND point_key LIKE ?",
                  (DbArg[2]){ DB_S(project_id), DB_S(like) }, 2, &r))
        return false;
    bool ok = db_rows_has_columns(&r, 1);
    size_t prefix = strlen(like) - 1;
    for (size_t i = 0; ok && i < r.nrows; i++) {
        const char *key;
        ok = db_cell_text(&r, i, 0, &key) && strlen(key) >= prefix;
        if (!ok) break;
        const char *h = key + prefix;
        bool current = false;
        for (size_t k = 0; k < n && !current; k++) current = !strcmp(piece_hashes[k], h);
        if (current) continue;
        /* This piece is gone from the document: its point and mark go too. */
        char name[160], path[260];
        collection_for(client, project_id, name, sizeof(name));
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
        ok = qdrant(client, "POST", path, body, NULL) && ok;
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

static size_t search_filtered(SemanticClient *client, const char *pid, const char *query, cJSON *filter, size_t limit, SemHit **out)
{
    *out = NULL;
    float *v = malloc(SEM_DIM * sizeof(float));
    char err[200];
    if (!v || !semantic_embed(client, &query, 1, true, v, err, sizeof(err))) {
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
    collection_for(client, pid, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s/points/search", name);
    cJSON *res = NULL;
    bool ok = qdrant(client, "POST", path, body, &res);
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

size_t semantic_canon_for(SemanticClient *client, const char *project_id, const char *statement, size_t limit, SemHit **out)
{
    /* Only committed canon: retracted entries and everything else (draft
     * text, notes, chat) are never treated as established fact. */
    cJSON *filter = cJSON_CreateObject();
    cJSON *must = cJSON_AddArrayToObject(filter, "must");
    cJSON_AddItemToArray(must, match("kind", "canon"));
    cJSON_AddItemToArray(must, match("status", "committed"));
    return search_filtered(client, project_id, statement, filter, limit, out);
}

size_t semantic_search(SemanticClient *client, const char *project_id, const char *query, const char *kind, size_t limit, SemHit **out)
{
    cJSON *filter = NULL;
    if (kind) {
        filter = cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_AddArrayToObject(filter, "must"), match("kind", kind));
    }
    return search_filtered(client, project_id, query, filter, limit, out);
}

bool semantic_drop(SemanticClient *client, const char *project_id)
{
    char name[160], path[220];
    collection_for(client, project_id, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s", name);
    bool ok = qdrant(client, "DELETE", path, NULL, NULL);
    Db *db = store_db(client->store);
    if (db) db_run(db, "DELETE FROM semantic_marks WHERE project_id = ?", (DbArg[1]){ DB_S(project_id) }, 1, NULL);
    return ok;
}

long semantic_rebuild(SemanticClient *client, const char *project_id, char *err, size_t errlen)
{
    Db *db = store_db(client->store);
    if (!db) {
        snprintf(err, errlen, "The project store is offline");
        return -1;
    }
    /* Pending until every part is indexed: an interrupted rebuild (an
     * embedding error, the app quitting) is resumed by the next sync. */
    set_rebuild_pending(db, project_id, true);
    semantic_drop(client, project_id); /* fine if it didn't exist */
    if (!ensure_collection(client, project_id)) {
        snprintf(err, errlen, "Qdrant isn't reachable");
        return -1;
    }
    if (!sync_records_impl(client, project_id, err, errlen)) return -1;
    long count = 0, failed = 0;
    DbRows r;
    /* Each document's current revision, in paragraphs. */
    if (db_query(db,
                 "SELECT d.id, v.id, v.content FROM documents d JOIN revisions v ON v.id = d.current_revision_id"
                 " WHERE d.project_id = ?",
                 (DbArg[1]){ DB_S(project_id) }, 1, &r)) {
        bool rows_ok = db_rows_has_columns(&r, 3);
        for (size_t i = 0; rows_ok && i < r.nrows; i++) {
            const char *document_id, *revision_id, *text;
            rows_ok = db_cell_text(&r, i, 0, &document_id) && db_cell_text(&r, i, 1, &revision_id) &&
                      db_cell_text(&r, i, 2, &text);
            if (!rows_ok) break;
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
                if (semantic_index_text(client, project_id, "draft", document_id, revision_id, NULL, 0, cps, clen, piece)) count++;
                else failed++;
                free(piece);
            }
            free(ps);
            free(pl);
        }
        if (!rows_ok) failed++;
        db_rows_free(&r);
    } else failed++;
    /* Chat messages (not failed replies or AI rewrites). */
    if (db_query(db,
                 "SELECT m.conversation_id, m.seq, m.content FROM messages m JOIN conversations c ON c.id = m.conversation_id"
                 " WHERE c.project_id = ? AND m.error = 0 AND NOT (m.role = 'assistant' AND m.is_edit = 1)",
                 (DbArg[1]){ DB_S(project_id) }, 1, &r)) {
        bool rows_ok = db_rows_has_columns(&r, 3);
        for (size_t i = 0; rows_ok && i < r.nrows; i++) {
            const char *conversation_id, *text;
            int64_t seq;
            rows_ok = db_cell_text(&r, i, 0, &conversation_id) && db_cell_i64(&r, i, 1, &seq) && seq >= 0 &&
                      seq <= LONG_MAX && db_cell_text(&r, i, 2, &text);
            if (!rows_ok) break;
            char *piece = malloc(2001);
            if (!piece) {
                failed++;
                continue;
            }
            utf8_clip(piece, 2001, text, 2000);
            size_t clen = 0; /* the range is in characters */
            for (const char *b = piece; *b; b++) clen += ((unsigned char)*b & 0xC0) != 0x80;
            if (semantic_index_text(client, project_id, "chat", NULL, NULL, conversation_id, (long)seq, 0, clen, piece))
                count++;
            else failed++;
            free(piece);
        }
        if (!rows_ok) failed++;
        db_rows_free(&r);
    } else failed++;
    DbRows rc;
    if (db_query(db, "SELECT COUNT(*) FROM records WHERE project_id = ? AND kind IN ('note','canon')",
                 (DbArg[1]){ DB_S(project_id) }, 1, &rc)) {
        int64_t records;
        if (rc.nrows == 1 && db_rows_has_columns(&rc, 1) && db_cell_i64(&rc, 0, 0, &records) && records >= 0 &&
            records <= LONG_MAX - count)
            count += (long)records;
        else
            failed++;
        db_rows_free(&rc);
    } else failed++;
    if (failed) {
        snprintf(err, errlen, "%ld passages couldn't be indexed; the rebuild resumes later", failed);
        return -1;
    }
    set_rebuild_pending(db, project_id, false);
    return count;
}

bool semantic_forget_collection(SemanticClient *client, const char *project_id)
{
    char name[160], path[220];
    collection_for(client, project_id, name, sizeof(name));
    snprintf(path, sizeof(path), "/collections/%s", name);
    return qdrant(client, "DELETE", path, NULL, NULL);
}

/* ---- private conversation-memory projection (never the legacy index) ---- */

static bool mv_fail(char *why, size_t n, const char *message)
{
    if (why && n) snprintf(why, n, "%s", message);
    return false;
}

static void mv_sha(const char *text, size_t n, char out[65])
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(text, (CC_LONG)n, digest);
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static bool mv_hex(const char *s, size_t cap)
{
    if (!s || strnlen(s, cap) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

bool memory_vectors_fingerprint(SemanticClient *client, char out[65])
{
    if (!client || !out) return false;
    const char *identity = "fake-bag-of-words-v1";
    if (client->embed) {
        const LlmServerConfig *cfg = llm_server_config(client->embed);
        LlmServerConfig pinned;
        llm_server_embed_config(&pinned, NULL);
        if (!cfg || !cfg->embedding || !mv_hex(cfg->sha256, sizeof(cfg->sha256)) || strcmp(cfg->sha256, pinned.sha256))
            return false;
        identity = cfg->sha256;
    }
    char key[512];
    /* The sequence projection changes search eligibility. A new fingerprint
     * makes old collections missing, so the worker reseeds SQL generations
     * before creating/reindexing; never silently query mixed payload formats. */
    snprintf(key, sizeof(key), "memory-v2|chat-sequence=int-v1|%s|dim=%d|pool=last|normalize=l2|query=%s", identity, SEM_DIM, k_query_instruction);
    mv_sha(key, strlen(key), out);
    return true;
}

static bool mv_identifier(const char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    if (!n || n == cap) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return false;
    return true;
}

static bool mv_scope(const MemoryScope *s)
{
    return s && mv_identifier(s->database, sizeof(s->database)) &&
           mv_identifier(s->id, sizeof(s->id)) && mv_identifier(s->project_id, sizeof(s->project_id)) &&
           mv_identifier(s->conversation_id, sizeof(s->conversation_id)) && s->history_epoch > 0 &&
           s->history_epoch <= INT64_MAX && s->policy_epoch > 0 && s->policy_epoch <= INT64_MAX &&
           s->generation > 0 && s->generation <= INT64_MAX;
}

/* store_new_id's typed immutable SQL identity, not a filename or free-form key. */
static bool mv_source_id(const char *id, size_t cap, char kind)
{
    if (!id || strnlen(id, cap) != 34 || id[0] != kind || id[1] != '-' || id[10] != '-' || id[17] != '-') return false;
    for (size_t i = 2; i < 34; i++) {
        if (i == 10 || i == 17) continue;
        if (i < 17 ? (id[i] < '0' || id[i] > '9') :
            !((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

static bool mv_documents(const MemoryScope *s, const MemoryDocumentSet *documents)
{
    if (!documents) return true;
    if (documents->count > MEMORY_DOCUMENTS_MAX) return false;
    for (size_t i = 0; i < documents->count; i++) {
        const MemoryDocumentRef *r = &documents->refs[i];
        if (strnlen(r->database, sizeof(r->database)) == sizeof(r->database) ||
            strnlen(r->project_id, sizeof(r->project_id)) == sizeof(r->project_id) ||
            strcmp(r->database, s->database) || strcmp(r->project_id, s->project_id) ||
            !mv_source_id(r->document_id, sizeof(r->document_id), 'd') ||
            !mv_source_id(r->revision_id, sizeof(r->revision_id), 'r') ||
            !mv_hex(r->source_hash, sizeof(r->source_hash))) return false;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(r->revision_id, documents->refs[j].revision_id)) return false;
    }
    return true;
}

static bool mv_path(SemanticClient *c, const MemoryScope *s, const char *suffix, char out[400])
{
    char database[64] = "", fp[65];
    if (!c || !mv_scope(s) || !memory_vectors_fingerprint(c, fp)) return false;
    if (!semantic_database(c, database, sizeof(database))) return false;
    if (strcmp(database, s->database)) return false;
    /* Admission checks the manager, but the endpoint belongs to the SQL
     * connection that produced the scope. Freeze it before any network I/O:
     * a concurrent database switch can never redirect old private text. */
    int n = snprintf(out, 400, "/collections/%s__%s__memory_v2__%s__%llu%s", s->database, s->project_id, fp,
                     (unsigned long long)s->generation, suffix);
    return n > 0 && n < 400;
}

/* Reject ambiguous duplicate keys and pathological response objects before
 * examining locators. Extra ordinary fields remain untrusted and ignored. */
static bool mv_json_shape(const cJSON *j, unsigned depth, size_t *budget)
{
    if (!j || !*budget || depth > 32) return false;
    --*budget;
    if (!cJSON_IsObject(j) && !cJSON_IsArray(j)) return true;
    size_t fields = 0;
    const cJSON *x;
    cJSON_ArrayForEach(x, j) {
        if (cJSON_IsObject(j)) {
            if (++fields > 128 || !x->string) return false;
            for (const cJSON *previous = j->child; previous != x; previous = previous->next)
                if (!strcmp(previous->string, x->string)) return false;
        }
        if (!mv_json_shape(x, depth + 1, budget)) return false;
    }
    return true;
}

/* Do not use the legacy helper: missing != unreachable and JSON must be exact.
 * http_request's tcp: transport is hardwired to loopback, never a remote URL. */
static bool mv_request(SemanticClient *c, const char *method, const char *path, cJSON *body, int *status, cJSON **out)
{
    *out = NULL;
    *status = 0;
    if (!c || !c->qkey[0]) return false;
    char *encoded = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body && !encoded) return false;
    HttpResult r = {0};
    bool ok = semantic_http(c, c->qport, method, path, c->qkey, encoded, 15000, &r);
    free(encoded);
    *status = r.status;
    if (ok && ((r.status >= 200 && r.status < 300) || r.status == 404)) {
        *out = r.body && r.len <= (1u << 20) ? json_parse_complete(r.body, r.len) : NULL;
        size_t budget = 8192;
        ok = cJSON_IsObject(*out) && mv_json_shape(*out, 0, &budget);
    } else ok = false;
    http_result_free(&r);
    return ok;
}

static bool mv_string(const cJSON *obj, const char *key, const char *value)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(x) && !strcmp(x->valuestring, value);
}

static bool mv_ok(const cJSON *j) { return mv_string(j, "status", "ok"); }

static bool mv_delete_namespace(SemanticClient *c, const char *path)
{
    cJSON *j = NULL;
    int status = 0;
    bool ok = mv_request(c, "DELETE", path, NULL, &status, &j);
    if (ok && status == 404) {
        /* A proxy's generic 404 is not proof that the exact collection is
         * missing. Require Qdrant's collection-specific missing response. */
        char error[480];
        snprintf(error, sizeof(error), "Not found: Collection `%s` doesn't exist!", path + strlen("/collections/"));
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(j, "status");
        ok = mv_string(result, "error", error);
    } else if (ok) {
        ok = mv_ok(j) && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "result"));
    }
    cJSON_Delete(j);
    return ok;
}

static bool mv_shape(const cJSON *j)
{
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(j, "result");
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(result, "config");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(config, "params");
    const cJSON *vectors = cJSON_GetObjectItemCaseSensitive(params, "vectors");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(vectors, "size");
    /* Named vectors have no top-level size/distance and are not compatible. */
    return mv_ok(j) && cJSON_IsObject(vectors) && cJSON_IsNumber(size) && size->valuedouble == SEM_DIM &&
           mv_string(vectors, "distance", "Cosine");
}

bool memory_vectors_retire_namespace_on_db(SemanticClient *c, Db *db, const MemoryNamespaceLease *l, char *why, size_t n)
{
    if (why && n) why[0] = '\0';
    char database[64] = "", path[400];
    /* Retirement authority is its SQL job, not a live conversation's epochs:
     * the originating scope may already have been deleted. */
    if (!c || !l || !mv_identifier(l->scope.database, sizeof(l->scope.database)) ||
        !mv_identifier(l->scope.project_id, sizeof(l->scope.project_id)) ||
        !mv_identifier(l->scope.id, sizeof(l->scope.id)) ||
        !mv_identifier(l->scope.conversation_id, sizeof(l->scope.conversation_id)) ||
        !l->scope.generation || l->scope.generation > INT64_MAX ||
        !mv_hex(l->embedding_fingerprint, sizeof(l->embedding_fingerprint)) ||
        (l->namespace_version != 1 && l->namespace_version != 2))
        return mv_fail(why, n, "Invalid retired memory namespace");
    if (!semantic_database(c, database, sizeof(database))) return mv_fail(why, n, "Retired namespace store unavailable");
    if (strcmp(database, l->scope.database)) return mv_fail(why, n, "Retired namespace database changed");
    int len = snprintf(path, sizeof(path), "/collections/%s__%s__memory_v%d__%s__%llu", l->scope.database,
                       l->scope.project_id, l->namespace_version, l->embedding_fingerprint,
                       (unsigned long long)l->scope.generation);
    if (len < 1 || (size_t)len >= sizeof(path)) return mv_fail(why, n, "Invalid retired namespace path");
    /* store_db returns this thread's connection. DATABASE() is verified again
     * inside the guard, so switching the manager cannot redirect authorization. */
    if (!memory_gc_namespace_prepare(db, l)) return mv_fail(why, n, "Retired namespace blocked or lease expired");
    /* The floor is committed. No SQL transaction or lock crosses HTTP. */
    bool ok = mv_delete_namespace(c, path);
    return ok || mv_fail(why, n, "Retired namespace deletion failed; retry is safe");
}

bool memory_vectors_retire_namespace(SemanticClient *c, const MemoryNamespaceLease *l, char *why, size_t n)
{
    if (!c) return mv_fail(why, n, "Invalid memory client");
    return memory_vectors_retire_namespace_on_db(c, store_db(c->store), l, why, n);
}

bool memory_vectors_exists(SemanticClient *c, const MemoryScope *s, bool *exists, char *why, size_t n)
{
    if (why && n) why[0] = '\0';
    if (exists) *exists = false;
    char path[400];
    if (!exists || !mv_path(c, s, "", path)) return mv_fail(why, n, "Invalid memory scope or embedding identity");
    cJSON *j = NULL;
    int status;
    bool ok = mv_request(c, "GET", path, NULL, &status, &j);
    if (ok && status != 404) {
        ok = mv_shape(j);
        *exists = ok;
    }
    cJSON_Delete(j);
    return ok || mv_fail(why, n, "Memory collection unavailable or incompatible");
}

bool memory_vectors_create_on_db(SemanticClient *c, Db *db, const MemoryScope *s, char *why, size_t n)
{
    char path[400];
    if (!s || !s->enabled || !mv_path(c, s, "", path)) return mv_fail(why, n, "Invalid or disabled memory scope");
    bool allowed = false, retired = false;
    if (!memory_gc_namespace_admit(db, s, &allowed, &retired) || !allowed)
        return mv_fail(why, n, "Memory namespace is no longer eligible");
    char fingerprint[65];
    if (!memory_vectors_fingerprint(c, fingerprint) || !memory_gc_namespace_enqueue(db, s, fingerprint, 2))
        return mv_fail(why, n, "Could not persist namespace retirement intent");
    bool exists = false;
    bool ok = memory_vectors_exists(c, s, &exists, why, n);
    if (!ok) return false;
    if (exists) {
        if (!memory_gc_namespace_admit(db, s, &allowed, &retired)) return mv_fail(why, n, "Memory namespace postcheck failed");
        if (retired) (void)mv_delete_namespace(c, path);
        return allowed || mv_fail(why, n, "Memory namespace retired during admission");
    }
    cJSON *body = cJSON_CreateObject(), *j = NULL;
    cJSON *vec = cJSON_AddObjectToObject(body, "vectors");
    ok = vec && cJSON_AddNumberToObject(vec, "size", SEM_DIM) && cJSON_AddStringToObject(vec, "distance", "Cosine");
    int status = 0;
    ok = ok && mv_request(c, "PUT", path, body, &status, &j) && status != 404 && mv_ok(j) &&
         cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "result"));
    cJSON_Delete(body);
    cJSON_Delete(j);
    /* A creator admitted before floor adoption may complete after DELETE.
     * Recheck even after an unacknowledged PUT (the server may have committed
     * it), and clean only a SQL-proven retired target using the pinned path. */
    if (!memory_gc_namespace_admit(db, s, &allowed, &retired)) return mv_fail(why, n, "Memory namespace postcheck failed");
    if (retired) (void)mv_delete_namespace(c, path);
    if (!allowed) return mv_fail(why, n, "Memory namespace retired during creation");
    /* A concurrent creator is safe only after validating its exact schema. */
    bool verified = false;
    if (!ok) return mv_fail(why, n, "Memory collection creation not acknowledged");
    ok = memory_vectors_exists(c, s, &verified, why, n) && verified;
    if (!memory_gc_namespace_admit(db, s, &allowed, &retired)) return mv_fail(why, n, "Memory namespace postcheck failed");
    if (retired) (void)mv_delete_namespace(c, path);
    return (ok && allowed) || mv_fail(why, n, "Memory namespace unavailable after creation");
}

bool memory_vectors_create(SemanticClient *c, const MemoryScope *s, char *why, size_t n)
{
    if (!c) return mv_fail(why, n, "Invalid memory client");
    return memory_vectors_create_on_db(c, store_db(c->store), s, why, n);
}

static bool mv_add_u64(cJSON *p, const char *key, uint64_t value)
{
    char text[24];
    snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    return cJSON_AddStringToObject(p, key, text) != NULL;
}

static cJSON *mv_scope_payload(const MemoryScope *s, const char *fp)
{
    cJSON *p = cJSON_CreateObject();
    bool ok = p && cJSON_AddStringToObject(p, "schema", "memory_v2") && cJSON_AddStringToObject(p, "embedding", fp) &&
              cJSON_AddStringToObject(p, "scope_id", s->id) && cJSON_AddStringToObject(p, "project_id", s->project_id) &&
              cJSON_AddStringToObject(p, "conversation_id", s->conversation_id) &&
              mv_add_u64(p, "history_epoch", s->history_epoch) && mv_add_u64(p, "policy_epoch", s->policy_epoch) &&
              mv_add_u64(p, "generation", s->generation);
    if (!ok) { cJSON_Delete(p); return NULL; }
    return p;
}

static bool mv_text(const char *text, size_t *bytes)
{
    if (!text) return false;
    size_t len = strnlen(text, MEMORY_UNIT_BYTES + 1);
    if (!len || len > MEMORY_UNIT_BYTES) return false;
    uint32_t cps[MEMORY_UNIT_BYTES];
    size_t invalid = 0;
    utf8_decode((const uint8_t *)text, len, cps, &invalid);
    if (invalid) return false;
    *bytes = len;
    return true;
}

static cJSON *mv_vector(const float v[SEM_DIM])
{
    cJSON *a = cJSON_CreateArray();
    if (!a) return NULL;
    for (int i = 0; i < SEM_DIM; i++) {
        cJSON *x = isfinite(v[i]) ? cJSON_CreateNumber(v[i]) : NULL;
        if (!x || !cJSON_AddItemToArray(a, x)) { cJSON_Delete(x); cJSON_Delete(a); return NULL; }
    }
    return a;
}

static bool mv_completed(const cJSON *j)
{
    return mv_ok(j) && mv_string(cJSON_GetObjectItemCaseSensitive(j, "result"), "status", "completed");
}

/* The unit itself, independent of any backend: shape, size, encoding, source
 * ids and the text hash. A unit that fails here will fail every time, so the
 * worker records it against the unit and moves on rather than counting it as
 * a backend failure (MEMORY-021: one malformed revision id halted the index). */
bool memory_vectors_unit_valid(const MemoryIndexLease *lease, char *why, size_t n)
{
    if (why && n) why[0] = '\0';
    char hash[65];
    size_t bytes;
    if (!lease || !lease->scope.enabled ||
        !mv_hex(lease->unit.id, sizeof(lease->unit.id)) || !mv_hex(lease->unit.text_hash, sizeof(lease->unit.text_hash)) ||
        !mv_text(lease->unit.text, &bytes) || bytes != lease->unit.text_bytes ||
         (lease->unit.kind != MEMORY_SOURCE_CHAT && lease->unit.kind != MEMORY_SOURCE_DOCUMENT) ||
         (lease->unit.kind == MEMORY_SOURCE_CHAT && (lease->unit.sequence < 0 || lease->unit.sequence > INT_MAX)) ||
        (lease->unit.kind == MEMORY_SOURCE_DOCUMENT &&
         (!mv_source_id(lease->unit.document_id, sizeof(lease->unit.document_id), 'd') ||
          !mv_source_id(lease->unit.revision_id, sizeof(lease->unit.revision_id), 'r'))))
        return mv_fail(why, n, "Invalid, oversized, or noncanonical memory unit");
    mv_sha(lease->unit.text, bytes, hash);
    if (strcmp(hash, lease->unit.text_hash)) return mv_fail(why, n, "Memory text hash mismatch");
    /* The experiment's index text (app/memory_index_text.h) is embedded in place of the text; it is bounded and
     * strict UTF-8 or the unit fails, never silently falls back. */
    size_t index_bytes;
    if (lease->unit.index_text && !memory_index_text_valid(lease->unit.index_text, &index_bytes))
        return mv_fail(why, n, "Invalid or oversized memory index text");
    return true;
}

bool memory_vectors_upsert(SemanticClient *c, const MemoryIndexLease *lease, char *why, size_t n)
{
    if (why && n) why[0] = '\0';
    char path[400], fp[65];
    if (!memory_vectors_unit_valid(lease, why, n)) return false;
    if (!mv_path(c, &lease->scope, "/points?wait=true", path)) return mv_fail(why, n, "Invalid, oversized, or noncanonical memory unit");
    bool exists;
    if (!memory_vectors_exists(c, &lease->scope, &exists, why, n)) return false;
    if (!exists) return mv_fail(why, n, "Memory collection lost; rebuild SQL generation before creating");
    memory_vectors_fingerprint(c, fp);
    float v[SEM_DIM];
    const char *text = lease->unit.index_text ? lease->unit.index_text : lease->unit.text;
    char err[256] = "";
    /* Already checked <=2000 bytes and strict UTF-8: legacy clipping is a no-op. */
    if (!semantic_embed(c, &text, 1, false, v, err, sizeof(err))) return mv_fail(why, n, err);
    cJSON *p = mv_scope_payload(&lease->scope, fp), *body = cJSON_CreateObject(), *pt = cJSON_CreateObject();
    bool ok = p && cJSON_AddStringToObject(p, "unit_id", lease->unit.id) &&
              cJSON_AddStringToObject(p, "text_hash", lease->unit.text_hash) &&
              cJSON_AddStringToObject(p, "source_kind", lease->unit.kind == MEMORY_SOURCE_DOCUMENT ? "document" : "chat");
    if (lease->unit.kind == MEMORY_SOURCE_DOCUMENT)
        ok = ok && cJSON_AddStringToObject(p, "document_id", lease->unit.document_id) &&
                    cJSON_AddStringToObject(p, "revision_id", lease->unit.revision_id);
    else ok = ok && cJSON_AddNumberToObject(p, "sequence", (double)lease->unit.sequence);
    char key[700], uuid[37];
    snprintf(key, sizeof(key), "%s|%s|%s|%s|%s|%s|%llu|%llu|%llu|%s", lease->scope.database, lease->scope.id, lease->scope.project_id,
             lease->scope.conversation_id, lease->unit.id, lease->unit.text_hash,
             (unsigned long long)lease->scope.history_epoch, (unsigned long long)lease->scope.policy_epoch,
             (unsigned long long)lease->scope.generation, fp);
    point_id(key, uuid);
    cJSON *vec = mv_vector(v), *pts = cJSON_AddArrayToObject(body, "points");
    ok = ok && pt && vec && pts && cJSON_AddStringToObject(pt, "id", uuid);
    if (ok && cJSON_AddItemToObject(pt, "payload", p)) p = NULL; else ok = false;
    if (ok && cJSON_AddItemToObject(pt, "vector", vec)) vec = NULL; else ok = false;
    if (ok && cJSON_AddItemToArray(pts, pt)) pt = NULL; else ok = false;
    cJSON *j = NULL;
    int status = 0;
    ok = ok && mv_request(c, "PUT", path, body, &status, &j) && status != 404 && mv_completed(j);
    cJSON_Delete(p); cJSON_Delete(vec); cJSON_Delete(pt); cJSON_Delete(body); cJSON_Delete(j);
    return ok || mv_fail(why, n, "Memory upsert not completed");
}

static cJSON *mv_filter(const cJSON *scope)
{
    cJSON *f = cJSON_CreateObject(), *must = cJSON_AddArrayToObject(f, "must");
    const cJSON *x;
    bool ok = must != NULL;
    cJSON_ArrayForEach(x, scope) {
        cJSON *m = cJSON_CreateObject(), *value = cJSON_AddObjectToObject(m, "match");
        if (!ok || !m || !value || !cJSON_AddStringToObject(m, "key", x->string) ||
            !cJSON_AddStringToObject(value, "value", x->valuestring) || !cJSON_AddItemToArray(must, m)) {
            cJSON_Delete(m); ok = false; break;
        }
    }
    if (!ok) { cJSON_Delete(f); return NULL; }
    return f;
}

/* This union is applied by Qdrant BEFORE its top-k limit; detached documents
 * must never crowd out authorized chat/document locators. */
static bool mv_sources_filter(cJSON *filter, const MemoryDocumentSet *documents, int64_t before)
{
    cJSON *should = cJSON_AddArrayToObject(filter, "should");
    if (!should) return false;
    size_t count = documents ? documents->count : 0;
    for (size_t i = 0; i <= count; i++) {
        cJSON *source = cJSON_CreateObject();
        bool ok = source && cJSON_AddStringToObject(source, "source_kind", i ? "document" : "chat");
        if (i) {
            const MemoryDocumentRef *r = &documents->refs[i - 1];
            ok = ok && cJSON_AddStringToObject(source, "document_id", r->document_id) &&
                       cJSON_AddStringToObject(source, "revision_id", r->revision_id);
        }
        cJSON *branch = ok ? mv_filter(source) : NULL;
        cJSON_Delete(source);
        if (branch && !i) {
            cJSON *must = cJSON_GetObjectItemCaseSensitive(branch, "must");
            cJSON *condition = cJSON_CreateObject();
            cJSON *range = condition ? cJSON_AddObjectToObject(condition, "range") : NULL;
            /* SQL sequences are <= INT_MAX; clamp the all-history sentinel to
             * an exactly represented JSON integer, rather than rounding int64. */
            double bound = before > INT_MAX ? (double)INT_MAX + 1 : (double)before;
            bool ranged = range && cJSON_AddStringToObject(condition, "key", "sequence") &&
                          cJSON_AddNumberToObject(range, "lt", bound) && cJSON_AddItemToArray(must, condition);
            if (!ranged) { cJSON_Delete(condition); cJSON_Delete(branch); return false; }
        }
        if (!branch || !cJSON_AddItemToArray(should, branch)) { cJSON_Delete(branch); return false; }
    }
    return true;
}

static bool mv_source_allowed(const cJSON *payload, const MemoryDocumentSet *documents, int64_t before)
{
    if (mv_string(payload, "source_kind", "chat")) {
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(payload, "sequence");
        return cJSON_IsNumber(seq) && isfinite(seq->valuedouble) && seq->valuedouble >= 0 &&
               seq->valuedouble <= INT_MAX && floor(seq->valuedouble) == seq->valuedouble &&
               (int64_t)seq->valuedouble < before &&
               !cJSON_HasObjectItem(payload, "document_id") && !cJSON_HasObjectItem(payload, "revision_id");
    }
    if (!mv_string(payload, "source_kind", "document") || !documents) return false;
    const cJSON *doc = cJSON_GetObjectItemCaseSensitive(payload, "document_id");
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(payload, "revision_id");
    if (!cJSON_IsString(doc) || !cJSON_IsString(rev) || !mv_source_id(doc->valuestring, 40, 'd') ||
        !mv_source_id(rev->valuestring, 40, 'r')) return false;
    for (size_t i = 0; i < documents->count; i++)
        if (!strcmp(doc->valuestring, documents->refs[i].document_id) &&
            !strcmp(rev->valuestring, documents->refs[i].revision_id)) return true;
    return false;
}

/* A unit has one immutable hash within a SQL generation. Conflicting versions
 * make the entire response ambiguous; identical repeats must not inflate RRF.
 * Preserve the first rank/score, rather than trusting repeated score claims. */
static bool mv_candidate_add(MemoryCandidate candidates[MEMORY_CANDIDATES], size_t *used,
                             const MemoryScope *s, const char *id, const char *hash, double score)
{
    for (size_t i = 0; i < *used; i++)
        if (!strcmp(candidates[i].unit_id, id)) return !strcmp(candidates[i].text_hash, hash);
    if (*used >= MEMORY_CANDIDATES) return false;
    MemoryCandidate *candidate = &candidates[(*used)++];
    memcpy(candidate->unit_id, id, 65);
    memcpy(candidate->text_hash, hash, 65);
    candidate->history_epoch = s->history_epoch;
    candidate->generation = s->generation;
    candidate->score = score;
    return true;
}

bool memory_vectors_search(SemanticClient *c, const MemoryScope *s, const char *query, size_t limit,
                           MemoryCandidate out[MEMORY_CANDIDATES], size_t *count, char *why, size_t n)
{
    return memory_vectors_search_with_documents(c, s, NULL, query, limit, out, count, why, n);
}

bool memory_vectors_search_with_documents(SemanticClient *c, const MemoryScope *s, const MemoryDocumentSet *documents,
                            const char *query, size_t limit, MemoryCandidate out[MEMORY_CANDIDATES], size_t *count, char *why, size_t n)
{
    return memory_vectors_search_with_documents_before(c, s, documents, query, INT64_MAX, limit, out, count, why, n);
}

bool memory_vectors_search_with_documents_before(SemanticClient *c, const MemoryScope *s, const MemoryDocumentSet *documents,
                            const char *query, int64_t before, size_t limit, MemoryCandidate out[MEMORY_CANDIDATES], size_t *count, char *why, size_t n)
{
    if (count) *count = 0;
    if (why && n) why[0] = '\0';
    char path[400], fp[65];
    size_t bytes;
    if (!count || !out || before < 0 || !limit || limit > MEMORY_CANDIDATES || !s || !s->enabled ||
        !mv_path(c, s, "/points/search", path) || !mv_text(query, &bytes) || !mv_documents(s, documents))
        return mv_fail(why, n, "Invalid memory search scope or query");
    bool exists;
    if (!memory_vectors_exists(c, s, &exists, why, n)) return false;
    if (!exists) return mv_fail(why, n, "Memory collection lost; SQL rebuild required");
    float v[SEM_DIM];
    char err[256] = "";
    if (!semantic_embed(c, &query, 1, true, v, err, sizeof(err))) return mv_fail(why, n, err);
    memory_vectors_fingerprint(c, fp);
    cJSON *scope = mv_scope_payload(s, fp), *filter = scope ? mv_filter(scope) : NULL;
    cJSON *body = cJSON_CreateObject(), *vec = mv_vector(v);
    bool ok = scope && body && filter && vec && mv_sources_filter(filter, documents, before) &&
              cJSON_AddNumberToObject(body, "limit", (double)limit) &&
              cJSON_AddBoolToObject(body, "with_payload", true);
    if (ok && cJSON_AddItemToObject(body, "vector", vec)) vec = NULL; else ok = false;
    if (ok && cJSON_AddItemToObject(body, "filter", filter)) filter = NULL; else ok = false;
    cJSON *j = NULL;
    int status = 0;
    ok = ok && mv_request(c, "POST", path, body, &status, &j) && status != 404 && mv_ok(j);
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "result"), *it;
    ok = ok && cJSON_IsArray(arr) && (size_t)cJSON_GetArraySize(arr) <= limit;
    MemoryCandidate candidates[MEMORY_CANDIDATES] = {0};
    size_t used = 0;
    cJSON_ArrayForEach(it, arr) {
        if (!ok) break;
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "payload"), *field;
        const cJSON *score = cJSON_GetObjectItemCaseSensitive(it, "score");
        ok = cJSON_IsObject(it) && cJSON_IsObject(p) && cJSON_IsNumber(score) && isfinite(score->valuedouble);
        cJSON_ArrayForEach(field, scope) if (!mv_string(p, field->string, field->valuestring)) ok = false;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(p, "unit_id"), *hash = cJSON_GetObjectItemCaseSensitive(p, "text_hash");
        ok = ok && cJSON_IsString(id) && mv_hex(id->valuestring, 65) && cJSON_IsString(hash) && mv_hex(hash->valuestring, 65) &&
              mv_source_allowed(p, documents, before);
        if (!ok) break;
        ok = mv_candidate_add(candidates, &used, s, id->valuestring, hash->valuestring, score->valuedouble);
    }
    cJSON_Delete(scope); cJSON_Delete(filter); cJSON_Delete(vec); cJSON_Delete(body); cJSON_Delete(j);
    if (!ok) return mv_fail(why, n, "Memory search rejected: malformed, conflicting, or wrong-scope locators (no partial results)");
    memcpy(out, candidates, used * sizeof(*out));
    *count = used;
    return true;
}

bool memory_vectors_drop(SemanticClient *c, const MemoryScope *s, char *why, size_t n)
{
    bool exists;
    if (!memory_vectors_exists(c, s, &exists, why, n)) return false;
    if (!exists) return true;
    char path[400], fp[65];
    if (!mv_path(c, s, "/points/delete?wait=true", path) || !memory_vectors_fingerprint(c, fp))
        return mv_fail(why, n, "Invalid memory scope");
    cJSON *scope = mv_scope_payload(s, fp), *filter = scope ? mv_filter(scope) : NULL;
    cJSON *body = cJSON_CreateObject(), *j = NULL;
    bool ok = body && filter && cJSON_AddItemToObject(body, "filter", filter);
    if (ok) filter = NULL;
    int status = 0;
    ok = ok && mv_request(c, "POST", path, body, &status, &j) && (status == 404 || mv_completed(j));
    cJSON_Delete(scope); cJSON_Delete(filter); cJSON_Delete(body); cJSON_Delete(j);
    return ok || mv_fail(why, n, "Memory scope deletion not completed");
}
