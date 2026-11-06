#define _DARWIN_C_SOURCE
#include "records.h"

#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/json.h"
#include "core/file_text.h"
#include "app/project_store.h"
#include "app/records_internal.h"
#include "app/store.h"

static bool log_path(const char *project_dir, RecordKind kind, bool create, char *out, size_t outlen)
{
    char base[1100];
    snprintf(base, sizeof(base), "%s/.scribodo", project_dir);
    if (create && mkdir(base, 0755) != 0 && errno != EEXIST) return false;
    snprintf(out, outlen, "%s/%s", base, kind == REC_NOTE ? "notes.jsonl" : kind == REC_CANON ? "canon.jsonl" : "findings.jsonl");
    return true;
}

static bool valid_id(const char *id)
{
    if (!id[0] || strlen(id) >= 40) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || *p == '-')) return false;
    return true;
}

void records_new_id(RecordKind kind, char *out, size_t outlen)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(out, outlen, "%c-%s-%04x", kind == REC_NOTE ? 'n' : kind == REC_CANON ? 'c' : 'f', stamp, arc4random_uniform(0x10000));
}

void records_free_sources(SourceRef *s, size_t n)
{
    for (size_t i = 0; i < n; i++) free(s[i].quote);
    free(s);
}

SourceRef *records_copy_sources(const SourceRef *s, size_t n)
{
    if (n == 0) return NULL;
    SourceRef *c = calloc(n, sizeof(SourceRef));
    if (!c) return NULL;
    for (size_t i = 0; i < n; i++) {
        c[i] = s[i];
        c[i].quote = strdup(s[i].quote ? s[i].quote : "");
        if (!c[i].quote) {
            records_free_sources(c, i);
            return NULL;
        }
    }
    return c;
}

static void free_record(Record *r)
{
    for (size_t v = 0; v < r->nversions; v++) {
        free(r->versions[v].content);
        free(r->versions[v].finding.related_text);
        records_free_sources(r->versions[v].sources, r->versions[v].nsources);
    }
    free(r->versions);
}

void records_free(Record *r, size_t n)
{
    for (size_t i = 0; i < n; i++) free_record(&r[i]);
    free(r);
}

/* A non-negative whole number field, or 0 if missing or out of range (a
 * missing field reads as NaN, which must never be cast to an integer). */
static double num(cJSON *j, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(j, key);
    double d = cJSON_IsNumber(v) ? v->valuedouble : 0.0;
    return d >= 0.0 && d < 1e15 ? d : 0.0;
}

static cJSON *source_json(const SourceRef *s)
{
    cJSON *j = cJSON_CreateObject();
    bool ok = j && cJSON_AddStringToObject(j, "kind", s->kind == SRC_DOC ? "doc" : "chat");
    if (s->kind == SRC_DOC) {
        ok = ok && cJSON_AddStringToObject(j, "doc", s->doc) && cJSON_AddStringToObject(j, "revision", s->revision);
    } else {
        ok = ok && cJSON_AddStringToObject(j, "chat", s->chat) && cJSON_AddNumberToObject(j, "message", (double)s->message);
    }
    ok = ok && cJSON_AddNumberToObject(j, "start", (double)s->start) && cJSON_AddNumberToObject(j, "len", (double)s->len) &&
         cJSON_AddStringToObject(j, "quote", s->quote ? s->quote : "");
    if (!ok) { cJSON_Delete(j); return NULL; }
    return j;
}

static bool parse_source(cJSON *j, SourceRef *s, bool *allocation_error)
{
    memset(s, 0, sizeof(*s));
    cJSON *kind = cJSON_GetObjectItem(j, "kind"), *quote = cJSON_GetObjectItem(j, "quote");
    if (!cJSON_IsString(kind) || !cJSON_IsString(quote)) return false;
    s->kind = strcmp(kind->valuestring, "chat") ? SRC_DOC : SRC_CHAT;
    cJSON *doc = cJSON_GetObjectItem(j, "doc"), *rev = cJSON_GetObjectItem(j, "revision"),
          *chat = cJSON_GetObjectItem(j, "chat");
    if (cJSON_IsString(doc)) snprintf(s->doc, sizeof(s->doc), "%s", doc->valuestring);
    if (cJSON_IsString(rev)) snprintf(s->revision, sizeof(s->revision), "%s", rev->valuestring);
    if (cJSON_IsString(chat)) snprintf(s->chat, sizeof(s->chat), "%s", chat->valuestring);
    /* File names from disk are never trusted as paths. */
    if (strchr(s->doc, '/') || strchr(s->revision, '/') || strchr(s->chat, '/')) return false;
    s->message = (long)num(j, "message");
    s->start = (size_t)num(j, "start");
    s->len = (size_t)num(j, "len");
    s->quote = strdup(quote->valuestring);
    if (!s->quote) *allocation_error = true;
    return s->quote != NULL;
}

typedef struct {
    Record r;
    size_t last_line; /* line number of its newest version (for ordering) */
} Slot;

static int by_recent(const void *a, const void *b)
{
    const Slot *x = a, *y = b;
    return x->last_line < y->last_line ? 1 : x->last_line > y->last_line ? -1 : 0;
}

static uint64_t id_hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

size_t records_list(StoreManager *manager, const char *project_dir, RecordKind kind, Record **out)
{
    return project_store_records_list(manager, project_dir, kind, out);
}

bool records_list_checked(StoreManager *manager, const char *project_dir, RecordKind kind, Record **out, size_t *count)
{
    return project_store_records_list_checked(manager, project_dir, kind, out, count);
}

bool records_list_files_checked(const char *project_dir, RecordKind kind, Record **out, size_t *count)
{
    /* Open-addressing index from id to slot (red-team 005: a linear search
     * made listing quadratic: 0.64 s for 20k findings on the UI thread). */
    size_t *index = NULL, index_cap = 0;
    *out = NULL;
    *count = 0;
    char path[1200];
    if (!log_path(project_dir, kind, false, path, sizeof(path))) return false;
    char *text = NULL;
    FileTextStatus read_status = file_text_read(path, &text);
    if (read_status != FILE_TEXT_OK) return read_status == FILE_TEXT_MISSING;
    if (json_has_nul_escape(text, strlen(text))) { free(text); return false; }
    Slot *slots = NULL;
    size_t n = 0, cap = 0, line_no = 0;
    char *save = NULL;
    bool ok = true;
    for (char *line = strtok_r(text, "\n", &save); ok && line; line = strtok_r(NULL, "\n", &save)) {
        line_no++;
        bool allocation_error;
        cJSON *j = json_parse_checked(line, &allocation_error);
        if (allocation_error) { cJSON_Delete(j); ok = false; break; }
        if (!j) continue; /* torn line after a crash */
        cJSON *id = cJSON_GetObjectItem(j, "id"), *content = cJSON_GetObjectItem(j, "content"),
              *status = cJSON_GetObjectItem(j, "status");
        if (!cJSON_IsString(id) || !valid_id(id->valuestring) || !cJSON_IsString(content) || !cJSON_IsString(status)) {
            cJSON_Delete(j);
            continue;
        }
        if ((n + 1) * 2 > index_cap) { /* grow and rehash */
            if (index_cap > SIZE_MAX / 2 / sizeof(size_t)) { cJSON_Delete(j); ok = false; break; }
            size_t ncap = index_cap ? index_cap * 2 : 64;
            size_t *ni = malloc(ncap * sizeof(size_t));
            if (!ni) {
                cJSON_Delete(j);
                ok = false;
                break;
            }
            for (size_t q = 0; q < ncap; q++) ni[q] = SIZE_MAX;
            for (size_t q = 0; q < n; q++) {
                size_t b = (size_t)(id_hash(slots[q].r.id) & (ncap - 1));
                while (ni[b] != SIZE_MAX) b = (b + 1) & (ncap - 1);
                ni[b] = q;
            }
            free(index);
            index = ni;
            index_cap = ncap;
        }
        size_t bucket = (size_t)(id_hash(id->valuestring) & (index_cap - 1)), k = n;
        while (index[bucket] != SIZE_MAX) {
            if (!strcmp(slots[index[bucket]].r.id, id->valuestring)) {
                k = index[bucket];
                break;
            }
            bucket = (bucket + 1) & (index_cap - 1);
        }
        if (k == n) {
            if (n == cap) {
                if (cap > SIZE_MAX / 2 / sizeof(Slot)) { cJSON_Delete(j); ok = false; break; }
                cap = cap ? cap * 2 : 16;
                Slot *s = realloc(slots, cap * sizeof(Slot));
                if (!s) {
                    cJSON_Delete(j);
                    ok = false;
                    break;
                }
                slots = s;
            }
            memset(&slots[n], 0, sizeof(Slot));
            snprintf(slots[n].r.id, sizeof(slots[n].r.id), "%s", id->valuestring);
            slots[n].r.created_seq = line_no;
            index[bucket] = n;
            n++;
        }
        Record *r = &slots[k].r;
        if (r->nversions == SIZE_MAX / sizeof(RecordVersion)) { cJSON_Delete(j); ok = false; break; }
        RecordVersion *vs = realloc(r->versions, (r->nversions + 1) * sizeof(RecordVersion));
        if (!vs) {
            cJSON_Delete(j);
            ok = false;
            break;
        }
        r->versions = vs;
        RecordVersion *v = &vs[r->nversions++];
        memset(v, 0, sizeof(*v));
        double ver = num(j, "ver");
        v->ver = ver < 1e9 ? (int)ver : 0;
        v->ts = (time_t)num(j, "ts");
        snprintf(v->status, sizeof(v->status), "%s", status->valuestring);
        v->content = strdup(content->valuestring);
        if (!v->content) ok = false;
        cJSON *fi = cJSON_GetObjectItem(j, "finding");
        if (cJSON_IsObject(fi)) {
            static const struct { const char *key; size_t off, len; } fields[] = {
                { "type", offsetof(FindingInfo, type), sizeof(((FindingInfo *)0)->type) },
                { "subject", offsetof(FindingInfo, subject), sizeof(((FindingInfo *)0)->subject) },
                { "relation", offsetof(FindingInfo, relation), sizeof(((FindingInfo *)0)->relation) },
                { "object", offsetof(FindingInfo, object), sizeof(((FindingInfo *)0)->object) },
                { "related", offsetof(FindingInfo, related), sizeof(((FindingInfo *)0)->related) },
            };
            for (size_t q = 0; q < sizeof(fields) / sizeof(fields[0]); q++) {
                cJSON *f = cJSON_GetObjectItem(fi, fields[q].key);
                if (cJSON_IsString(f)) snprintf((char *)&v->finding + fields[q].off, fields[q].len, "%s", f->valuestring);
            }
            cJSON *rt = cJSON_GetObjectItem(fi, "related_text");
            if (cJSON_IsString(rt)) {
                v->finding.related_text = strdup(rt->valuestring);
                if (!v->finding.related_text) ok = false;
            }
        }
        cJSON *srcs = cJSON_GetObjectItem(j, "sources");
        int ns = cJSON_IsArray(srcs) ? cJSON_GetArraySize(srcs) : 0;
        if (ns > 0) v->sources = calloc((size_t)ns, sizeof(SourceRef));
        if (ns > 0 && !v->sources) ok = false;
        for (int i = 0; ok && i < ns; i++) {
            bool source_allocation_error = false;
            if (parse_source(cJSON_GetArrayItem(srcs, i), &v->sources[v->nsources], &source_allocation_error)) v->nsources++;
            if (source_allocation_error) ok = false;
        }
        slots[k].last_line = line_no;
        cJSON_Delete(j);
    }
    free(text);
    free(index);
    qsort(slots, n, sizeof(Slot), by_recent);
    Record *list = ok && n ? malloc(n * sizeof(Record)) : NULL;
    if (n && !list) ok = false;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (ok && slots[i].r.nversions) list[m++] = slots[i].r;
        else free_record(&slots[i].r);
    }
    free(slots);
    if (!ok) { free(list); return false; }
    *out = list;
    *count = m;
    return true;
}

size_t records_list_files(const char *project_dir, RecordKind kind, Record **out)
{
    size_t count = 0;
    (void)records_list_files_checked(project_dir, kind, out, &count);
    return count;
}

bool records_append(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                    const SourceRef *sources, size_t nsources, int *ver_out)
{
    return records_append_ex(manager, project_dir, kind, id, status, content, sources, nsources, NULL, ver_out);
}

static bool append_impl(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                        const SourceRef *sources, size_t nsources, const FindingInfo *finding, int *ver_out, bool fresh);

bool records_append_ex(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                       const SourceRef *sources, size_t nsources, const FindingInfo *finding, int *ver_out)
{
    return append_impl(manager, project_dir, kind, id, status, content, sources, nsources, finding, ver_out, false);
}

bool records_create_ex(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                       const SourceRef *sources, size_t nsources, const FindingInfo *finding)
{
    return append_impl(manager, project_dir, kind, id, status, content, sources, nsources, finding, NULL, true);
}

static bool append_impl(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                         const SourceRef *sources, size_t nsources, const FindingInfo *finding, int *ver_out, bool fresh)
{
    if (ver_out) *ver_out = 0;
    if (!valid_id(id)) return false;
    return project_store_records_append(manager, project_dir, kind, id, status, content, sources, nsources, finding, fresh, ver_out);
}

bool records_append_files(StoreManager *manager, const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                          const SourceRef *sources, size_t nsources, const FindingInfo *finding, bool fresh, int *ver_out)
{
    char path[1200];
    if (!log_path(project_dir, kind, true, path, sizeof(path))) return false;

    /* Next version number: one past the newest stored (a fresh id is 1, with
     * no need to read the whole log). */
    int ver = 1;
    if (!fresh) {
        Record *list = NULL;
        size_t n = 0;
        if (!records_list_files_checked(project_dir, kind, &list, &n)) return false;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(list[i].id, id)) ver = record_current(&list[i])->ver + 1;
        records_free(list, n);
    }

    cJSON *j = cJSON_CreateObject();
    bool built = j && cJSON_AddStringToObject(j, "id", id) && cJSON_AddNumberToObject(j, "ver", ver) &&
                 cJSON_AddNumberToObject(j, "ts", (double)(time_t)store_write_time(manager)) &&
                 cJSON_AddStringToObject(j, "status", status) && cJSON_AddStringToObject(j, "content", content ? content : "");
    cJSON *arr = built ? cJSON_AddArrayToObject(j, "sources") : NULL;
    built = built && arr;
    for (size_t i = 0; built && i < nsources; i++) {
        cJSON *source = source_json(&sources[i]);
        built = source && cJSON_AddItemToArray(arr, source);
        if (!built) cJSON_Delete(source);
    }
    if (built && finding) {
        cJSON *fi = cJSON_AddObjectToObject(j, "finding");
        built = fi && cJSON_AddStringToObject(fi, "type", finding->type);
        if (built && finding->subject[0]) built = cJSON_AddStringToObject(fi, "subject", finding->subject);
        if (built && finding->relation[0]) built = cJSON_AddStringToObject(fi, "relation", finding->relation);
        if (built && finding->object[0]) built = cJSON_AddStringToObject(fi, "object", finding->object);
        if (built && finding->related[0]) built = cJSON_AddStringToObject(fi, "related", finding->related);
        if (built && finding->related_text) built = cJSON_AddStringToObject(fi, "related_text", finding->related_text);
    }
    char *s = built ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!s) return false;
    size_t len = strlen(s);
    char *line = malloc(len + 2);
    if (!line) {
        free(s);
        return false;
    }
    memcpy(line, s, len);
    line[len] = '\n';
    free(s);

    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    bool ok = fd >= 0;
    if (ok) {
        /* Terminate a torn line from a crash, then one write per record. */
        off_t end = lseek(fd, 0, SEEK_END);
        char last = '\n';
        if (end > 0 && pread(fd, &last, 1, end - 1) == 1 && last != '\n') ok = write(fd, "\n", 1) == 1;
        ok = ok && write(fd, line, len + 1) == (ssize_t)(len + 1) && fsync(fd) == 0;
        ok = close(fd) == 0 && ok;
    }
    free(line);
    if (ok && ver_out) *ver_out = ver;
    return ok;
}
