/* Isolated loopback HTTP fixture; no database, Qdrant, model, or author data.
 * Link like test_llm (full core/store objects), with this file as sole main. */
#define _DARWIN_C_SOURCE
#include "test_util.h"
#include "core/json.h"
#include "llm/memory_vectors.h"
#include "llm/http.h"
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Linked only with tests/memory_gc_stubs.c, never production SQL authority. */
extern void memory_gc_test_reset(void);
extern void memory_gc_test_authorize(const MemoryScope *, const char *fingerprint);
extern void memory_gc_test_register(const MemoryNamespaceLease *);
extern void memory_gc_test_advance_floor(uint64_t);
extern unsigned memory_gc_test_ack_count(void);

enum { NORMAL, BAD_SHAPE, BAD_METRIC, NAMED_VECTOR, ACK_ONLY, BAD_RESULT, WRONG_SCOPE,
       OLD_GENERATION, OLD_POLICY, NUMBER_EPOCH, BAD_SCORE, TOO_MANY, EMBED_DIM, EMBED_NAN, TRAILING_JSON,
       WRONG_DIM, INFINITE_SCORE, DUPLICATE_KEY, BAD_UNIT_ID, BAD_HASH, WRONG_EMBEDDING,
       NUL_LOCATOR, NUL_FIELD, DUPLICATE_CONFLICT, MIXED_INVALID, MISSING_KIND, WRONG_KIND, WRONG_REVISION,
         MISSING_SEQUENCE, FRACTIONAL_SEQUENCE, NEGATIVE_SEQUENCE, TOO_LARGE_SEQUENCE,
         DUPLICATE_IDENTICAL, RETIRE_DURING_CREATE, REVOKE_DURING_EXISTS,
        DELETE_FALSE, DELETE_GENERIC_404, DELETE_EXACT_404, DELETE_WRONG_404 };
typedef struct {
    int fd, port;
    pthread_t thread;
    pthread_mutex_t lock;
    atomic_bool stop;
    atomic_int mode;
    bool exists;
    cJSON *points[8];
    size_t count;
    char last_id[40], last_path[400], last_embed[2400];
    int upserts, last_limit, bad_requests, requests, embeds, last_sources, namespace_deletes;
    uint64_t retire_generation;
    StoreManager *switch_during_embed;
    bool switched;
} Stub;

static bool send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t sent = send(fd, p, n, 0);
        if (sent <= 0) return false;
        p += sent; n -= (size_t)sent;
    }
    return true;
}

static bool filter_matches(const cJSON *payload, const cJSON *filter)
{
    const cJSON *must = cJSON_GetObjectItem(filter, "must"), *it;
    if (!cJSON_IsArray(must)) return false;
    cJSON_ArrayForEach(it, must) {
        const cJSON *key = cJSON_GetObjectItem(it, "key"), *match = cJSON_GetObjectItem(it, "match");
        const cJSON *value = cJSON_GetObjectItem(match, "value");
        const cJSON *actual = cJSON_IsString(key) ? cJSON_GetObjectItem(payload, key->valuestring) : NULL;
        const cJSON *range = cJSON_GetObjectItem(it, "range");
        if (range) {
            const cJSON *lt = cJSON_GetObjectItem(range, "lt");
            if (!cJSON_IsNumber(lt) || !cJSON_IsNumber(actual) || !(actual->valuedouble < lt->valuedouble)) return false;
            continue;
        }
        if (!cJSON_IsString(value) || !cJSON_IsString(actual) || strcmp(value->valuestring, actual->valuestring)) return false;
    }
    const cJSON *should = cJSON_GetObjectItem(filter, "should");
    if (should) {
        bool found = false;
        cJSON_ArrayForEach(it, should) if (filter_matches(payload, it)) found = true;
        if (!found) return false;
    }
    return true;
}
static bool filtered(const cJSON *payload, const cJSON *body)
{
    const cJSON *filter = cJSON_GetObjectItem(body, "filter");
    const cJSON *must = cJSON_GetObjectItem(filter, "must");
    return cJSON_IsArray(must) && cJSON_GetArraySize(must) == 8 && filter_matches(payload, filter);
}

static char *response(Stub *s, const char *method, const char *path, cJSON *body, int *status)
{
    int mode = atomic_load(&s->mode);
    s->requests++;
    *status = 200;
    snprintf(s->last_path, sizeof(s->last_path), "%s", path);
    if (strstr(path, "/v1/embeddings")) {
        s->embeds++;
        if (s->switch_during_embed) {
            char why[256], database[64];
            /* Forced-offline store_start changes configuration without opening
             * SQL. This gate runs before returning the embedding response. */
            store_set_offline(s->switch_during_embed, true);
            store_start(s->switch_during_embed, "switched_database", why, sizeof(why));
            store_database(s->switch_during_embed, database, sizeof(database));
            s->switched = !strcmp(database, "switched_database");
            s->switch_during_embed = NULL;
        }
        cJSON *input = cJSON_GetArrayItem(cJSON_GetObjectItem(body, "input"), 0);
        if (!cJSON_IsString(input)) s->bad_requests++;
        else snprintf(s->last_embed, sizeof(s->last_embed), "%s", input->valuestring);
        cJSON *j = cJSON_CreateObject(), *data = cJSON_AddArrayToObject(j, "data"), *item = cJSON_CreateObject();
        cJSON_AddItemToArray(data, item);
        cJSON_AddNumberToObject(item, "index", 0);
        cJSON *v = cJSON_AddArrayToObject(item, "embedding");
        for (int i = 0; i < SEM_DIM - (mode == EMBED_DIM); i++)
            cJSON_AddItemToArray(v, mode == EMBED_NAN && !i ? cJSON_CreateString("NaN") : cJSON_CreateNumber(i == 0));
        char *encoded = cJSON_PrintUnformatted(j); cJSON_Delete(j); return encoded;
    }
    if (!strstr(path, "__memory_v2__")) s->bad_requests++;
    if (!strcmp(method, "GET")) {
        if (!s->exists) { *status = 404; return strdup("{}"); }
        if (mode == REVOKE_DURING_EXISTS) memory_gc_test_reset();
        if (mode == BAD_SHAPE) return strdup("{\"status\":\"ok\",\"result\":{}}");
        if (mode == BAD_METRIC) return strdup("{\"status\":\"ok\",\"result\":{\"config\":{\"params\":{\"vectors\":{\"size\":1024,\"distance\":\"Dot\"}}}}}");
        if (mode == NAMED_VECTOR) return strdup("{\"status\":\"ok\",\"result\":{\"config\":{\"params\":{\"vectors\":{\"named\":{\"size\":1024,\"distance\":\"Cosine\"}}}}}}");
        if (mode == WRONG_DIM) return strdup("{\"status\":\"ok\",\"result\":{\"config\":{\"params\":{\"vectors\":{\"size\":768,\"distance\":\"Cosine\"}}}}}");
        return strdup("{\"status\":\"ok\",\"result\":{\"config\":{\"params\":{\"vectors\":{\"size\":1024,\"distance\":\"Cosine\"}}}}}");
    }
    if (!strstr(path, "/points")) {
        if (!strcmp(method, "DELETE")) {
            ++s->namespace_deletes;
            if (mode == DELETE_FALSE) return strdup("{\"status\":\"ok\",\"result\":false}");
            if (mode == DELETE_GENERIC_404) { *status = 404; return strdup("{}"); }
            if (mode == DELETE_EXACT_404 || mode == DELETE_WRONG_404) {
                char encoded[600];
                *status = 404;
                snprintf(encoded, sizeof(encoded),
                    "{\"status\":{\"error\":\"Not found: Collection `%s` doesn't exist!\"}}",
                    mode == DELETE_EXACT_404 ? path + strlen("/collections/") : "other_collection");
                return strdup(encoded);
            }
            s->exists = false;
        } else if (!strcmp(method, "PUT")) {
            s->exists = true;
            if (mode == RETIRE_DURING_CREATE) memory_gc_test_advance_floor(s->retire_generation);
        } else {
            ++s->bad_requests;
        }
        return strdup("{\"status\":\"ok\",\"result\":true}");
    }
    if (strstr(path, "/points/search")) {
        if (mode == BAD_RESULT) return strdup("{\"status\":\"ok\",\"result\":{}}");
        if (mode == TRAILING_JSON) return strdup("{\"status\":\"ok\",\"result\":[]} trailing");
        if (mode == DUPLICATE_KEY) return strdup("{\"status\":\"ok\",\"result\":[],\"result\":{}}");
        cJSON *limit = cJSON_GetObjectItem(body, "limit");
        s->last_limit = cJSON_IsNumber(limit) ? limit->valueint : 0;
        const cJSON *filter = cJSON_GetObjectItem(body, "filter");
        s->last_sources = cJSON_GetArraySize(cJSON_GetObjectItem(filter, "should"));
        if (s->last_sources < 1 || s->last_sources > 9) s->bad_requests++;
        cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "status", "ok");
        cJSON *arr = cJSON_AddArrayToObject(j, "result");
        size_t n = mode == TOO_MANY ? MEMORY_CANDIDATES + 1 : s->count;
        for (size_t i = 0; i < n; i++) {
            const cJSON *original = s->points[i % (s->count ? s->count : 1)];
            if (!original) continue;
            cJSON *p = cJSON_GetObjectItem(original, "payload");
            bool injected = mode >= WRONG_SCOPE && mode <= TOO_MANY;
            if (!injected && !filtered(p, body)) continue;
            cJSON *hit = cJSON_CreateObject(), *copy = cJSON_Duplicate(p, true);
            cJSON_AddItemToObject(hit, "payload", copy);
            /* Even a valid locator may carry attacker-controlled source text. */
            cJSON_AddStringToObject(copy, "text", "INJECTED PRIVATE SOURCE, NEVER HYDRATE");
            if (mode == WRONG_SCOPE) cJSON_ReplaceItemInObject(copy, "conversation_id", cJSON_CreateString("other-conversation"));
            if (mode == OLD_GENERATION) cJSON_ReplaceItemInObject(copy, "generation", cJSON_CreateString("1"));
            if (mode == OLD_POLICY) cJSON_ReplaceItemInObject(copy, "policy_epoch", cJSON_CreateString("1"));
            if (mode == NUMBER_EPOCH) cJSON_ReplaceItemInObject(copy, "history_epoch", cJSON_CreateNumber(9007199254740992.0));
            if (mode == BAD_UNIT_ID) cJSON_ReplaceItemInObject(copy, "unit_id", cJSON_CreateString("not-a-sha256"));
            if (mode == BAD_HASH) cJSON_ReplaceItemInObject(copy, "text_hash", cJSON_CreateString("NOT-LOWERCASE-HEX"));
            if (mode == WRONG_EMBEDDING) cJSON_ReplaceItemInObject(copy, "embedding", cJSON_CreateString("wrong-embedding-fingerprint"));
            if (mode == MISSING_KIND) cJSON_DeleteItemFromObject(copy, "source_kind");
            if (mode == MISSING_SEQUENCE) cJSON_DeleteItemFromObject(copy, "sequence");
            if (mode == FRACTIONAL_SEQUENCE) cJSON_ReplaceItemInObject(copy, "sequence", cJSON_CreateNumber(0.5));
            if (mode == NEGATIVE_SEQUENCE) cJSON_ReplaceItemInObject(copy, "sequence", cJSON_CreateNumber(-1));
            if (mode == TOO_LARGE_SEQUENCE) cJSON_ReplaceItemInObject(copy, "sequence", cJSON_CreateNumber(2147483648.0));
            if (mode == WRONG_KIND) cJSON_ReplaceItemInObject(copy, "source_kind", cJSON_CreateString("document"));
            if (mode == WRONG_REVISION) {
                cJSON_ReplaceItemInObject(copy, "source_kind", cJSON_CreateString("document"));
                cJSON_DeleteItemFromObject(copy, "document_id");
                cJSON_DeleteItemFromObject(copy, "revision_id");
                cJSON_AddStringToObject(copy, "document_id", "d-20261107-120000-0000000000000000");
                cJSON_AddStringToObject(copy, "revision_id", "r-20261107-120000-0000000000000001");
            }
            if (mode == NUL_LOCATOR) {
                const cJSON *unit = cJSON_GetObjectItem(copy, "unit_id");
                char raw[100]; snprintf(raw, sizeof(raw), "\"%s\\u0000evil\"", unit->valuestring);
                cJSON_ReplaceItemInObject(copy, "unit_id", cJSON_CreateRaw(raw));
            }
            if (mode == NUL_FIELD) cJSON_AddRawToObject(copy, "untrusted", "{\"field\\u0000suffix\":\"ignored\"}");
            if (mode == BAD_SCORE) cJSON_AddStringToObject(hit, "score", "NaN");
            else if (mode == INFINITE_SCORE) cJSON_AddRawToObject(hit, "score", "1e999");
            else cJSON_AddNumberToObject(hit, "score", 0.9);
            cJSON_AddItemToArray(arr, hit);
            if (mode == DUPLICATE_CONFLICT || mode == MIXED_INVALID || mode == DUPLICATE_IDENTICAL) {
                cJSON *duplicate = cJSON_Duplicate(hit, true);
                cJSON *payload = cJSON_GetObjectItem(duplicate, "payload");
                if (mode == DUPLICATE_CONFLICT) cJSON_ReplaceItemInObject(payload, "text_hash",
                    cJSON_CreateString("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
                if (mode == MIXED_INVALID) cJSON_ReplaceItemInObject(payload, "text_hash", cJSON_CreateString("invalid-hash"));
                cJSON_ReplaceItemInObject(duplicate, "score", cJSON_CreateNumber(0.2));
                cJSON_AddItemToArray(arr, duplicate);
            }
        }
        char *encoded = cJSON_PrintUnformatted(j); cJSON_Delete(j); return encoded;
    }
    if (!strstr(path, "wait=true")) s->bad_requests++;
    if (strstr(path, "/points/delete")) {
        for (size_t i = 0; i < s->count;) {
            if (filtered(cJSON_GetObjectItem(s->points[i], "payload"), body)) {
                cJSON_Delete(s->points[i]); s->points[i] = s->points[--s->count]; s->points[s->count] = NULL;
            } else i++;
        }
    } else {
        cJSON *point = cJSON_GetArrayItem(cJSON_GetObjectItem(body, "points"), 0);
        cJSON *id = cJSON_GetObjectItem(point, "id"), *p = cJSON_GetObjectItem(point, "payload");
        cJSON *kind = cJSON_GetObjectItem(p, "source_kind");
        bool document = cJSON_IsString(kind) && !strcmp(kind->valuestring, "document");
        if (!cJSON_IsString(id) || !cJSON_IsString(kind) || cJSON_HasObjectItem(p, "text") ||
            cJSON_HasObjectItem(p, "document_name") || cJSON_GetArraySize(p) != (document ? 13 : 12)) s->bad_requests++;
        if (cJSON_IsString(id)) {
            snprintf(s->last_id, sizeof(s->last_id), "%s", id->valuestring);
            size_t i;
            for (i = 0; i < s->count; i++) {
                cJSON *old = cJSON_GetObjectItem(s->points[i], "id");
                if (cJSON_IsString(old) && !strcmp(old->valuestring, id->valuestring)) break;
            }
            if (i < 8) {
                cJSON_Delete(s->points[i]); s->points[i] = cJSON_Duplicate(point, true);
                if (i == s->count) s->count++;
            }
            s->upserts++;
        }
    }
    return strdup(mode == ACK_ONLY ? "{\"status\":\"ok\",\"result\":{\"status\":\"acknowledged\"}}" :
                                     "{\"status\":\"ok\",\"result\":{\"status\":\"completed\"}}");
}

static void *serve(void *arg)
{
    Stub *s = arg;
    while (!atomic_load(&s->stop)) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
        if (poll(&pfd, 1, 10) <= 0) continue;
        int fd = accept(s->fd, NULL, NULL);
        if (fd < 0) continue;
        int one = 1;
        struct timeval timeout = { .tv_sec = 5 };
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char request[65536] = "";
        size_t used = 0, need = sizeof(request);
        char *body = NULL;
        while (used < sizeof(request) - 1 && used < need) {
            ssize_t n = recv(fd, request + used, sizeof(request) - 1 - used, 0);
            if (n <= 0) break;
            used += (size_t)n; request[used] = '\0';
            char *end = strstr(request, "\r\n\r\n"), *cl = strstr(request, "Content-Length:");
            if (end) { body = end + 4; need = (size_t)(body - request) + (cl ? strtoul(cl + 15, NULL, 10) : 0); }
        }
        char method[16] = "", path[400] = "";
        sscanf(request, "%15s %399s", method, path);
        cJSON *j = body && used == need && *body ? json_parse_complete(body, strlen(body)) : NULL;
        pthread_mutex_lock(&s->lock);
        int status;
        char *encoded = response(s, method, path, j, &status);
        pthread_mutex_unlock(&s->lock);
        cJSON_Delete(j);
        char header[256];
        size_t len = strlen(encoded);
        int n = snprintf(header, sizeof(header), "HTTP/1.1 %d OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, len);
        if (send_all(fd, header, (size_t)n)) send_all(fd, encoded, len);
        free(encoded); close(fd);
    }
    return NULL;
}

static bool start(Stub *s)
{
    memset(s, 0, sizeof(*s)); atomic_init(&s->stop, false); atomic_init(&s->mode, NORMAL);
    pthread_mutex_init(&s->lock, NULL);
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (s->fd < 0 || bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(s->fd, 4)) return false;
    socklen_t len = sizeof(addr);
    if (getsockname(s->fd, (struct sockaddr *)&addr, &len)) return false;
    s->port = ntohs(addr.sin_port);
    return pthread_create(&s->thread, NULL, serve, s) == 0;
}

static void sha(const char *text, char out[65])
{
    unsigned char digest[32]; CC_SHA256(text, (CC_LONG)strlen(text), digest);
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static int request_count(Stub *stub)
{
    pthread_mutex_lock(&stub->lock);
    int requests = stub->requests;
    pthread_mutex_unlock(&stub->lock);
    return requests;
}

static void namespace_guards(Stub *stub, SemanticClient *client, const MemoryScope *scope, const char *fingerprint)
{
    char why[256];
    memory_gc_test_reset();
    int before = request_count(stub);
    CHECK(!memory_vectors_create(client, scope, why, sizeof(why)) && why[0]);
    CHECK(request_count(stub) == before); /* Default deny, even with NULL SQL. */

    memory_gc_test_authorize(scope, fingerprint);
    atomic_store(&stub->mode, REVOKE_DURING_EXISTS);
    pthread_mutex_lock(&stub->lock);
    stub->exists = true;
    pthread_mutex_unlock(&stub->lock);
    CHECK(!memory_vectors_create(client, scope, why, sizeof(why)) && why[0]);
    CHECK(request_count(stub) == before + 1); /* Post-GET authority revocation. */

    memory_gc_test_authorize(scope, fingerprint);
    atomic_store(&stub->mode, RETIRE_DURING_CREATE);
    pthread_mutex_lock(&stub->lock);
    stub->exists = false;
    stub->retire_generation = scope->generation;
    int deletes = stub->namespace_deletes;
    pthread_mutex_unlock(&stub->lock);
    CHECK(!memory_vectors_create(client, scope, why, sizeof(why)) && why[0]);
    pthread_mutex_lock(&stub->lock);
    CHECK(!stub->exists && stub->namespace_deletes == deletes + 1);
    CHECK(!strstr(stub->last_path, "/points"));
    pthread_mutex_unlock(&stub->lock);
    before = request_count(stub);
    CHECK(!memory_vectors_create(client, scope, why, sizeof(why)));
    CHECK(request_count(stub) == before); /* Retired floor never publishes ready. */

    memory_gc_test_authorize(scope, fingerprint);
    MemoryNamespaceLease retired = { .id = 1, .fence = 4, .namespace_version = 2, .scope = *scope };
    strcpy(retired.owner, "namespace-worker");
    strcpy(retired.embedding_fingerprint, fingerprint);
    --retired.scope.generation;
    memory_gc_test_register(&retired);
    MemoryNamespaceLease forged = retired;
    strcpy(forged.scope.project_id, "wrong-project");
    CHECK(!memory_vectors_retire_namespace(client, &forged, why, sizeof(why)));
    forged = retired;
    strcpy(forged.scope.database, "other_database");
    CHECK(!memory_vectors_retire_namespace(client, &forged, why, sizeof(why)));
    forged = retired;
    forged.embedding_fingerprint[0] = fingerprint[0] == 'a' ? 'b' : 'a';
    CHECK(!memory_vectors_retire_namespace(client, &forged, why, sizeof(why)));
    forged = retired;
    forged.namespace_version = 1;
    CHECK(!memory_vectors_retire_namespace(client, &forged, why, sizeof(why)));
    forged = retired;
    ++forged.fence;
    CHECK(!memory_vectors_retire_namespace(client, &forged, why, sizeof(why)));
    CHECK(request_count(stub) == before); /* Forgery fails before network I/O. */

    MemoryNamespaceLease active = retired;
    active.scope.generation = scope->generation;
    memory_gc_test_register(&active);
    CHECK(!memory_vectors_retire_namespace(client, &active, why, sizeof(why)));
    CHECK(request_count(stub) == before); /* Even registered active targets fail. */
    memory_gc_test_register(&retired);

    const int rejected[] = { DELETE_FALSE, DELETE_GENERIC_404, DELETE_WRONG_404 };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        atomic_store(&stub->mode, rejected[i]);
        bool delivered = memory_vectors_retire_namespace(client, &retired, why, sizeof(why));
        if (delivered) (void)memory_gc_namespace_ack(NULL, &retired, true);
        CHECK(!delivered && why[0]);
        CHECK(memory_gc_test_ack_count() == 0);
    }
    atomic_store(&stub->mode, DELETE_EXACT_404);
    CHECK(memory_vectors_retire_namespace(client, &retired, why, sizeof(why)));
    CHECK(memory_gc_namespace_ack(NULL, &retired, true) && memory_gc_test_ack_count() == 1);
    atomic_store(&stub->mode, NORMAL);
    CHECK(memory_vectors_retire_namespace(client, &retired, why, sizeof(why)));
    CHECK(memory_gc_namespace_ack(NULL, &retired, true) && memory_gc_test_ack_count() == 2);
    pthread_mutex_lock(&stub->lock);
    char expected[400];
    snprintf(expected, sizeof(expected), "/collections/%s__%s__memory_v2__%s__%llu", scope->database,
             scope->project_id, fingerprint, (unsigned long long)retired.scope.generation);
    CHECK(!strcmp(stub->last_path, expected));
    stub->exists = true;
    pthread_mutex_unlock(&stub->lock);
    memory_gc_test_authorize(scope, fingerprint);
}

#ifdef SCRIBODO_FAULT_ALLOC_H
/* Also build this fixture with -include tests/fault_alloc.h and json_vendor.c,
 * just like test_semantic_faults. Failures are thread-local, so the HTTP stub
 * remains healthy while every client allocation is failed once/persistently. */
enum { ALLOC_EXISTS, ALLOC_CREATE, ALLOC_UPSERT, ALLOC_UPSERT_DOCUMENT, ALLOC_SEARCH, ALLOC_SEARCH_DOCUMENTS, ALLOC_DROP };

static bool allocation_call(int operation, SemanticClient *client, const MemoryIndexLease *lease,
                            MemoryCandidate out[MEMORY_CANDIDATES], size_t *count, bool *exists, char why[256])
{
    MemoryIndexLease document = *lease;
    MemoryDocumentSet documents = { .count = MEMORY_DOCUMENTS_MAX };
    for (size_t i = 0; i < documents.count; i++) {
        MemoryDocumentRef *r = &documents.refs[i];
        strcpy(r->database, lease->scope.database); strcpy(r->project_id, lease->scope.project_id);
        snprintf(r->document_id, sizeof(r->document_id), "d-20261107-120000-%016zx", i);
        snprintf(r->revision_id, sizeof(r->revision_id), "r-20261107-120000-%016zx", i);
        strcpy(r->source_hash, lease->unit.text_hash);
    }
    document.unit.kind = MEMORY_SOURCE_DOCUMENT;
    strcpy(document.unit.document_id, documents.refs[0].document_id);
    strcpy(document.unit.revision_id, documents.refs[0].revision_id);
    sha("document-unit", document.unit.id);
    switch (operation) {
    case ALLOC_EXISTS: return memory_vectors_exists(client, &lease->scope, exists, why, 256);
    case ALLOC_CREATE: return memory_vectors_create(client, &lease->scope, why, 256);
    case ALLOC_UPSERT: return memory_vectors_upsert(client, lease, why, 256);
    case ALLOC_UPSERT_DOCUMENT: return memory_vectors_upsert(client, &document, why, 256);
    case ALLOC_SEARCH: return memory_vectors_search(client, &lease->scope, "lantern", 16, out, count, why, 256);
    case ALLOC_SEARCH_DOCUMENTS: return memory_vectors_search_with_documents(client, &lease->scope, &documents,
                                                                            "lantern", 16, out, count, why, 256);
    default: return memory_vectors_drop(client, &lease->scope, why, 256);
    }
}

static void allocation_prepare(int operation, Stub *stub, SemanticClient *client, const MemoryIndexLease *lease)
{
    fault_alloc_after(-1);
    pthread_mutex_lock(&stub->lock); stub->exists = operation != ALLOC_CREATE; pthread_mutex_unlock(&stub->lock);
    if (operation == ALLOC_DROP) {
        char why[256];
        CHECK(memory_vectors_upsert(client, lease, why, sizeof(why)));
    }
}

/* The sweeps assume one fixed allocation sequence per call. The HTTP decoder
 * used to grow its body buffer per delivered piece, so the count depended on
 * how the kernel segmented a response; a hosted run then never reached the
 * injected position and the call succeeded (AUDIT-002). Content-Length bodies
 * must allocate the same way whether they arrive whole, in KiB pieces or by byte. */
static long decoder_allocations(const char *response, size_t len, size_t body_len, size_t piece)
{
    HttpDecoder d;
    http_decoder_init(&d, NULL, NULL);
    fault_alloc_after(-1);
    bool fed = true;
    for (size_t off = 0; off < len && fed; off += piece) {
        size_t take = len - off < piece ? len - off : piece;
        fed = http_decoder_feed(&d, response + off, take);
    }
    long calls = fault_alloc_count();
    fault_alloc_after(-1);
    CHECK(fed && d.done && d.status == 200 && d.body_len == body_len && d.body && d.body[0] == 'x');
    http_decoder_free(&d);
    return calls;
}

static void decoder_delivery(void)
{
    enum { BODY = 5000 };
    char header[128];
    int hlen = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", BODY);
    size_t len = (size_t)hlen + BODY;
    char *response = malloc(len);
    CHECK(hlen > 0 && response);
    if (!response) return;
    memcpy(response, header, (size_t)hlen);
    memset(response + hlen, 'x', BODY);
    long whole = decoder_allocations(response, len, BODY, len);
    long kib = decoder_allocations(response, len, BODY, 1024);
    long bytes = decoder_allocations(response, len, BODY, 1);
    if (whole != kib || whole != bytes)
        fprintf(stderr, "decoder allocations: whole=%ld 1KiB=%ld bytewise=%ld\n", whole, kib, bytes);
    CHECK(whole > 0 && whole == kib && whole == bytes);
    free(response);
    /* A declared length is not a promise: one byte behind a 64 MiB Content-Length
     * must not reserve more than the presize bound (doubling may overshoot it). */
    HttpDecoder d;
    http_decoder_init(&d, NULL, NULL);
    const char *lying = "HTTP/1.1 200 OK\r\nContent-Length: 67108864\r\n\r\nx";
    CHECK(http_decoder_feed(&d, lying, strlen(lying)) && d.body_len == 1);
    if (d.body_cap > (2u << 20)) fprintf(stderr, "decoder reserved %zu bytes for a one-byte body\n", d.body_cap);
    CHECK(d.body_cap <= (2u << 20));
    http_decoder_free(&d);
}

static void allocation_sweeps(Stub *stub, SemanticClient *client, const MemoryIndexLease *lease)
{
    for (int operation = ALLOC_EXISTS; operation <= ALLOC_DROP; operation++) {
        allocation_prepare(operation, stub, client, lease);
        MemoryCandidate out[MEMORY_CANDIDATES], sentinel[MEMORY_CANDIDATES];
        memset(sentinel, 0xa5, sizeof(sentinel));
        size_t count = 99;
        bool exists = true;
        char why[256] = "";
        fault_alloc_after(-1);
        bool ok = allocation_call(operation, client, lease, out, &count, &exists, why);
        long allocations = fault_alloc_count();
        fault_alloc_after(-1);
        CHECK(ok && allocations > 0);
        for (long i = 0; i < allocations * 2; i++) {
            allocation_prepare(operation, stub, client, lease);
            memcpy(out, sentinel, sizeof(out)); count = 99; exists = true; why[0] = '\0';
            if (i < allocations) fault_alloc_once_after(i);
            else fault_alloc_after(i - allocations);
            ok = allocation_call(operation, client, lease, out, &count, &exists, why);
            long calls = fault_alloc_count(), failures = fault_alloc_failures();
            fault_alloc_after(-1);
            if (ok || !why[0] || ((operation == ALLOC_SEARCH || operation == ALLOC_SEARCH_DOCUMENTS) &&
                (count || memcmp(out, sentinel, sizeof(out)))))
                fprintf(stderr, "allocation diagnostic: operation=%d position=%ld mode=%s control_calls=%ld actual_calls=%ld actual_failures=%ld ok=%d count=%zu why=%s\n",
                    operation, i < allocations ? i : i - allocations, i < allocations ? "once" : "persistent",
                    allocations, calls, failures, ok, count, why);
            CHECK(!ok && why[0]);
            if (operation == ALLOC_SEARCH || operation == ALLOC_SEARCH_DOCUMENTS) CHECK(!count && !memcmp(out, sentinel, sizeof(out)));
            if (operation == ALLOC_EXISTS) CHECK(!exists);
        }
        printf("    vector allocation operation=%d allocations=%ld (once + persistent)\n", operation, allocations);
    }
    char why[256];
    CHECK(memory_vectors_upsert(client, lease, why, sizeof(why)));
}
#endif

static void projection(void)
{
    memory_gc_test_reset();
    const char *base = getenv("TMPDIR");
    char root[512]; snprintf(root, sizeof(root), "%s/scribodo-vectors-XXXXXX", base ? base : "/tmp");
    CHECK(mkdtemp(root));
    char keypath[600]; snprintf(keypath, sizeof(keypath), "%s/qdrant_api_key", root);
    FILE *f = fopen(keypath, "wb"); CHECK(f);
    if (!f) return;
    fputs("isolated-test-key", f); fclose(f);
    char dbkeypath[600]; snprintf(dbkeypath, sizeof(dbkeypath), "%s/db_password", root);
    f = fopen(dbkeypath, "wb"); CHECK(f);
    if (!f) return;
    fputs("isolated-not-a-real-database-password", f); fclose(f);
    Stub stub;
    CHECK(start(&stub));
    char port[16]; snprintf(port, sizeof(port), "%d", stub.port);
    setenv("SCRIBODO_SECRETS", root, 1); setenv("SCRIBODO_QDRANT_PORT", port, 1);
    StoreManager *store = store_manager_create();
    SemanticClient *client = semantic_client_create(store, NULL);
    CHECK(client);
    MemoryIndexLease lease = { .scope = { .database = "scribodo", .id = "scope-a", .project_id = "project-a", .conversation_id = "conversation-a",
        .history_epoch = 9007199254740993ULL, .policy_epoch = 3, .generation = 7, .enabled = true } };
    const char *text = "Mara remembers Zoë's violet lantern.";
    lease.unit.text = (char *)text; lease.unit.text_bytes = strlen(text);
    sha("unit-identity", lease.unit.id); sha(text, lease.unit.text_hash);
    char why[256], id[40], fakefp[65], realfp[65];
    bool exists = true;
    MemoryIndexLease mismatch = lease;
    snprintf(mismatch.scope.database, sizeof(mismatch.scope.database), "other_database");
    MemoryCandidate rejected[MEMORY_CANDIDATES]; size_t rejected_count = 99;
    pthread_mutex_lock(&stub.lock); int requests_before = stub.requests, embeds_before = stub.embeds; pthread_mutex_unlock(&stub.lock);
    CHECK(!memory_vectors_exists(client, &mismatch.scope, &exists, why, sizeof(why)) && !exists);
    CHECK(!memory_vectors_create(client, &mismatch.scope, why, sizeof(why)));
    CHECK(!memory_vectors_upsert(client, &mismatch, why, sizeof(why)));
    CHECK(!memory_vectors_search(client, &mismatch.scope, "private query", 16, rejected, &rejected_count, why, sizeof(why)) && !rejected_count);
    CHECK(!memory_vectors_drop(client, &mismatch.scope, why, sizeof(why)));
    pthread_mutex_lock(&stub.lock); CHECK(stub.requests == requests_before && stub.embeds == embeds_before); pthread_mutex_unlock(&stub.lock);
    CHECK(memory_vectors_exists(client, &lease.scope, &exists, why, sizeof(why)) && !exists);
    CHECK(!memory_vectors_upsert(client, &lease, why, sizeof(why)));
    CHECK(memory_vectors_fingerprint(client, fakefp));
    namespace_guards(&stub, client, &lease.scope, fakefp);
    CHECK(memory_vectors_create(client, &lease.scope, why, sizeof(why)));
    CHECK(memory_vectors_create(client, &lease.scope, why, sizeof(why)));
    CHECK(memory_vectors_upsert(client, &lease, why, sizeof(why)));
    pthread_mutex_lock(&stub.lock); snprintf(id, sizeof(id), "%s", stub.last_id); pthread_mutex_unlock(&stub.lock);
    CHECK(memory_vectors_upsert(client, &lease, why, sizeof(why)));
    pthread_mutex_lock(&stub.lock); CHECK(!strcmp(id, stub.last_id) && stub.count == 1); pthread_mutex_unlock(&stub.lock);
    const int64_t invalid_sequences[] = {-1, (int64_t)INT_MAX + 1, INT64_MAX};
    for (size_t i = 0; i < sizeof(invalid_sequences)/sizeof(invalid_sequences[0]); i++) {
        MemoryIndexLease invalid = lease;
        invalid.unit.sequence = invalid_sequences[i];
        pthread_mutex_lock(&stub.lock); int before = stub.requests; pthread_mutex_unlock(&stub.lock);
        CHECK(!memory_vectors_unit_valid(&invalid, why, sizeof(why)) && why[0]);
        CHECK(!memory_vectors_upsert(client, &invalid, why, sizeof(why)) && why[0]);
        pthread_mutex_lock(&stub.lock); CHECK(stub.requests == before); pthread_mutex_unlock(&stub.lock);
        CHECK(memory_vectors_upsert(client, &lease, why, sizeof(why))); /* restore for independent probes */
    }
    MemoryIndexLease maximum_sequence = lease;
    maximum_sequence.unit.sequence = INT_MAX;
    CHECK(memory_vectors_upsert(client, &maximum_sequence, why, sizeof(why)));
    MemoryCandidate boundary_candidates[MEMORY_CANDIDATES]; size_t boundary_count = 99;
    CHECK(memory_vectors_search_with_documents_before(client, &lease.scope, NULL, "lantern", INT_MAX, 16,
              boundary_candidates, &boundary_count, why, sizeof(why)) && boundary_count == 0);
    CHECK(memory_vectors_search_with_documents_before(client, &lease.scope, NULL, "lantern", INT64_MAX, 16,
              boundary_candidates, &boundary_count, why, sizeof(why)) && boundary_count == 1);
    CHECK(memory_vectors_upsert(client, &lease, why, sizeof(why)));
    MemoryCandidate out[MEMORY_CANDIDATES]; size_t count = 99;
    CHECK(memory_vectors_search(client, &lease.scope, "violet lantern", 16, out, &count, why, sizeof(why)) && count == 1);
    CHECK(!strcmp(out[0].unit_id, lease.unit.id) && !strcmp(out[0].text_hash, lease.unit.text_hash));
    CHECK(out[0].history_epoch == lease.scope.history_epoch && out[0].generation == 7);
    pthread_mutex_lock(&stub.lock); CHECK(stub.last_limit == 16); pthread_mutex_unlock(&stub.lock);
#ifdef SCRIBODO_FAULT_ALLOC_H
    allocation_sweeps(&stub, client, &lease);
#endif
    MemoryScope other = lease.scope; snprintf(other.id, sizeof(other.id), "scope-b");
    snprintf(other.conversation_id, sizeof(other.conversation_id), "conversation-b");
    CHECK(memory_vectors_search(client, &other, "lantern", 16, out, &count, why, sizeof(why)) && !count);
    other = lease.scope; other.generation++;
    CHECK(memory_vectors_search(client, &other, "lantern", 16, out, &count, why, sizeof(why)) && !count);
    other = lease.scope; other.policy_epoch++;
    CHECK(memory_vectors_search(client, &other, "lantern", 16, out, &count, why, sizeof(why)) && !count);
    for (int mode = BAD_SHAPE; mode < DUPLICATE_IDENTICAL; mode++) {
        if (mode == ACK_ONLY || mode == EMBED_DIM || mode == EMBED_NAN) continue;
        atomic_store(&stub.mode, mode); count = 99;
        CHECK(!memory_vectors_search(client, &lease.scope, "lantern", 16, out, &count, why, sizeof(why)) && count == 0 && why[0]);
    }
    atomic_store(&stub.mode, DUPLICATE_IDENTICAL);
    CHECK(memory_vectors_search(client, &lease.scope, "lantern", 16, out, &count, why, sizeof(why)) && count == 1);
    CHECK(!strcmp(out[0].unit_id, lease.unit.id) && out[0].score == 0.9);
    atomic_store(&stub.mode, ACK_ONLY);
    CHECK(!memory_vectors_upsert(client, &lease, why, sizeof(why)));
    CHECK(!memory_vectors_drop(client, &lease.scope, why, sizeof(why)));
    atomic_store(&stub.mode, NORMAL);
    CHECK(memory_vectors_upsert(client, &lease, why, sizeof(why)));
    /* Exact revision admission happens before ANN top-k, with a single shared
     * limit. Same text/hash in a different revision is not authorization. */
    MemoryDocumentSet documents = { .count = MEMORY_DOCUMENTS_MAX };
    for (size_t i = 0; i < documents.count; i++) {
        MemoryDocumentRef *r = &documents.refs[i];
        strcpy(r->database, lease.scope.database); strcpy(r->project_id, lease.scope.project_id);
        snprintf(r->document_id, sizeof(r->document_id), "d-20261107-120000-%016zx", i);
        snprintf(r->revision_id, sizeof(r->revision_id), "r-20261107-120000-%016zx", i);
        strcpy(r->source_hash, lease.unit.text_hash); r->bytes = lease.unit.text_bytes;
        strcpy(r->name, "<|system|> filename must never enter payload");
    }
    MemoryIndexLease document = lease;
    document.unit.kind = MEMORY_SOURCE_DOCUMENT;
    strcpy(document.unit.document_id, documents.refs[0].document_id);
    strcpy(document.unit.revision_id, documents.refs[0].revision_id);
    strcpy(document.unit.document_name, documents.refs[0].name);
    sha("document-unit", document.unit.id);
    CHECK(memory_vectors_upsert(client, &document, why, sizeof(why)));
    CHECK(memory_vectors_search(client, &lease.scope, "lantern", 16, out, &count, why, sizeof(why)) && count == 1);
    CHECK(memory_vectors_search_with_documents(client, &lease.scope, &documents, "lantern", 32, out, &count, why, sizeof(why)) && count == 2);
    CHECK(memory_vectors_search_with_documents_before(client, &lease.scope, &documents, "lantern", 0, 32,
                                                      out, &count, why, sizeof(why)) && count == 1);
    CHECK(!strcmp(out[0].unit_id, document.unit.id)); /* cutoff zero still permits saved documents */
    CHECK(memory_vectors_search_with_documents_before(client, &lease.scope, &documents, "lantern", 1, 32,
                                                      out, &count, why, sizeof(why)) && count == 2);
    CHECK(!memory_vectors_search_with_documents_before(client, &lease.scope, &documents, "lantern", -1, 32,
                                                      out, &count, why, sizeof(why)) && count == 0);
    pthread_mutex_lock(&stub.lock); CHECK(stub.last_sources == 9 && stub.last_limit == 32); pthread_mutex_unlock(&stub.lock);
    documents.refs[0].revision_id[33] = 'f';
    CHECK(memory_vectors_search_with_documents(client, &lease.scope, &documents, "lantern", 16, out, &count, why, sizeof(why)) && count == 1);
    documents.refs[0].revision_id[33] = '0';
    atomic_store(&stub.mode, WRONG_REVISION);
    documents.count = 1;
    memset(out, 0xa5, sizeof(out)); MemoryCandidate untouched[MEMORY_CANDIDATES]; memcpy(untouched, out, sizeof(out));
    CHECK(!memory_vectors_search_with_documents(client, &lease.scope, &documents, "lantern", 16, out, &count, why, sizeof(why)) && !count);
    CHECK(!memcmp(out, untouched, sizeof(out)));
    atomic_store(&stub.mode, NORMAL);
    documents.count = MEMORY_DOCUMENTS_MAX + 1;
    CHECK(!memory_vectors_search_with_documents(client, &lease.scope, &documents, "lantern", 16, out, &count, why, sizeof(why)) && !count);
    documents.count = 1; strcpy(documents.refs[0].database, "detached_database");
    CHECK(!memory_vectors_search_with_documents(client, &lease.scope, &documents, "lantern", 16, out, &count, why, sizeof(why)) && !count);
    document.unit.document_id[0] = 'r';
    CHECK(!memory_vectors_upsert(client, &document, why, sizeof(why)));
    document.unit.document_id[0] = 'd'; document.unit.kind = (MemorySourceKind)99;
    CHECK(!memory_vectors_upsert(client, &document, why, sizeof(why)));
    /* Leave the remaining legacy projection checks with their one chat point. */
    pthread_mutex_lock(&stub.lock);
    /* An earlier failed upsert leaves no point to remove; do not index -1. */
    CHECK(stub.count > 0);
    if (stub.count > 0) { cJSON_Delete(stub.points[--stub.count]); stub.points[stub.count] = NULL; }
    pthread_mutex_unlock(&stub.lock);
    CHECK(!memory_vectors_search(client, &lease.scope, "query", 0, out, &count, why, sizeof(why)) && !count);
    CHECK(!memory_vectors_search(client, &lease.scope, "query", 33, out, &count, why, sizeof(why)) && !count);
    char large[MEMORY_UNIT_BYTES + 2]; memset(large, 'x', sizeof(large) - 1); large[sizeof(large) - 1] = '\0';
    CHECK(!memory_vectors_search(client, &lease.scope, large, 16, out, &count, why, sizeof(why)));
    CHECK(!memory_vectors_search(client, &lease.scope, "bad\xed\xa0\x80", 16, out, &count, why, sizeof(why)));
    lease.unit.text = large; lease.unit.text_bytes = strlen(large); sha(large, lease.unit.text_hash);
    CHECK(!memory_vectors_upsert(client, &lease, why, sizeof(why)));
    lease.unit.text = "bad\xc0\xaf"; lease.unit.text_bytes = strlen(lease.unit.text); sha(lease.unit.text, lease.unit.text_hash);
    CHECK(!memory_vectors_upsert(client, &lease, why, sizeof(why)));
    lease.unit.text = (char *)text; lease.unit.text_bytes = strlen(text);
    CHECK(!memory_vectors_upsert(client, &lease, why, sizeof(why))); /* stale hash */
    sha(text, lease.unit.text_hash);
    /* Pinned real server uses a different collection; malformed embedding
     * shape never makes it to upsert, and valid text is embedded exactly. */
    LlmServerConfig cfg; llm_server_embed_config(&cfg, NULL);
    LlmServer *server = llm_server_create(&cfg);
    char socket_path[32]; snprintf(socket_path, sizeof(socket_path), "tcp:%d", stub.port);
    llm_server_test_attach(server, socket_path, "embed-key");
    SemanticClient *real = semantic_client_create(store, server);
    CHECK(memory_vectors_fingerprint(client, fakefp) && memory_vectors_fingerprint(real, realfp) && strcmp(fakefp, realfp));
    CHECK(memory_vectors_upsert(real, &lease, why, sizeof(why)));
    pthread_mutex_lock(&stub.lock);
    CHECK(!strcmp(stub.last_embed, text) && strstr(stub.last_path, realfp));
    pthread_mutex_unlock(&stub.lock);
    large[MEMORY_UNIT_BYTES] = '\0'; lease.unit.text = large; lease.unit.text_bytes = MEMORY_UNIT_BYTES;
    sha(large, lease.unit.text_hash);
    CHECK(memory_vectors_upsert(real, &lease, why, sizeof(why)));
    pthread_mutex_lock(&stub.lock); CHECK(strlen(stub.last_embed) == MEMORY_UNIT_BYTES && !strcmp(stub.last_embed, large)); pthread_mutex_unlock(&stub.lock);
    lease.unit.text = (char *)text; lease.unit.text_bytes = strlen(text); sha(text, lease.unit.text_hash);
    atomic_store(&stub.mode, EMBED_DIM); CHECK(!memory_vectors_upsert(real, &lease, why, sizeof(why)));
    atomic_store(&stub.mode, EMBED_NAN); CHECK(!memory_vectors_upsert(real, &lease, why, sizeof(why)));
    atomic_store(&stub.mode, NORMAL);
    pthread_mutex_lock(&stub.lock); stub.switch_during_embed = store; pthread_mutex_unlock(&stub.lock);
    CHECK(memory_vectors_upsert(real, &lease, why, sizeof(why)));
    pthread_mutex_lock(&stub.lock);
    CHECK(stub.switched && strstr(stub.last_path, "/collections/scribodo__project-a__memory_v2__") == stub.last_path);
    CHECK(!strstr(stub.last_path, "switched_database"));
    requests_before = stub.requests; embeds_before = stub.embeds;
    pthread_mutex_unlock(&stub.lock);
    CHECK(!memory_vectors_upsert(real, &lease, why, sizeof(why)));
    pthread_mutex_lock(&stub.lock); CHECK(stub.requests == requests_before && stub.embeds == embeds_before); pthread_mutex_unlock(&stub.lock);
    /* Restore the manager only, without touching any database service. */
    store_start(store, "scribodo", why, sizeof(why));
    char database[64]; store_database(store, database, sizeof(database)); CHECK(!strcmp(database, "scribodo"));
    semantic_client_destroy(real); llm_server_destroy(server);
    /* Scoped cleanup must retain another conversation in the same collection. */
    other = lease.scope; snprintf(other.id, sizeof(other.id), "scope-b");
    snprintf(other.conversation_id, sizeof(other.conversation_id), "conversation-b");
    MemoryIndexLease second = lease; second.scope = other;
    CHECK(memory_vectors_upsert(client, &second, why, sizeof(why)));
    CHECK(memory_vectors_drop(client, &lease.scope, why, sizeof(why)));
    CHECK(memory_vectors_search(client, &lease.scope, "lantern", 16, out, &count, why, sizeof(why)) && !count);
    CHECK(memory_vectors_search(client, &other, "lantern", 16, out, &count, why, sizeof(why)) && count == 1);
    pthread_mutex_lock(&stub.lock); stub.exists = false; pthread_mutex_unlock(&stub.lock);
    CHECK(memory_vectors_exists(client, &other, &exists, why, sizeof(why)) && !exists);
    CHECK(!memory_vectors_upsert(client, &second, why, sizeof(why)));
    CHECK(!memory_vectors_search(client, &other, "lantern", 16, out, &count, why, sizeof(why)) && !count);
    CHECK(memory_vectors_drop(client, &other, why, sizeof(why)));
    pthread_mutex_lock(&stub.lock); CHECK(stub.bad_requests == 0); pthread_mutex_unlock(&stub.lock);
    semantic_client_destroy(client); store_manager_destroy(store);
    atomic_store(&stub.stop, true); pthread_join(stub.thread, NULL); close(stub.fd);
    for (size_t i = 0; i < stub.count; i++) cJSON_Delete(stub.points[i]);
    pthread_mutex_destroy(&stub.lock); unlink(keypath); unlink(dbkeypath); rmdir(root);
    memory_gc_test_reset();
}

int main(void)
{
    const TestCase tests[] = { { "private memory vector projection", projection },
#ifdef SCRIBODO_FAULT_ALLOC_H
                               { "decoder allocations independent of delivery", decoder_delivery },
#endif
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
