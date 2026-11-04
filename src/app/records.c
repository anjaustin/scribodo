#define _DARWIN_C_SOURCE
#include "records.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

static bool log_path(const char *project_dir, RecordKind kind, bool create, char *out, size_t outlen)
{
    char base[1100];
    snprintf(base, sizeof(base), "%s/.scribodo", project_dir);
    if (create && mkdir(base, 0755) != 0 && errno != EEXIST) return false;
    snprintf(out, outlen, "%s/%s", base, kind == REC_NOTE ? "notes.jsonl" : "canon.jsonl");
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
    snprintf(out, outlen, "%c-%s-%04x", kind == REC_NOTE ? 'n' : 'c', stamp, arc4random_uniform(0x10000));
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (buf) buf[fread(buf, 1, (size_t)n, f)] = '\0';
    fclose(f);
    return buf;
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
    cJSON_AddStringToObject(j, "kind", s->kind == SRC_DOC ? "doc" : "chat");
    if (s->kind == SRC_DOC) {
        cJSON_AddStringToObject(j, "doc", s->doc);
        cJSON_AddStringToObject(j, "revision", s->revision);
    } else {
        cJSON_AddStringToObject(j, "chat", s->chat);
        cJSON_AddNumberToObject(j, "message", (double)s->message);
    }
    cJSON_AddNumberToObject(j, "start", (double)s->start);
    cJSON_AddNumberToObject(j, "len", (double)s->len);
    cJSON_AddStringToObject(j, "quote", s->quote ? s->quote : "");
    return j;
}

static bool parse_source(cJSON *j, SourceRef *s)
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

size_t records_list(const char *project_dir, RecordKind kind, Record **out)
{
    *out = NULL;
    char path[1200];
    if (!log_path(project_dir, kind, false, path, sizeof(path))) return 0;
    char *text = slurp(path);
    if (!text) return 0;
    Slot *slots = NULL;
    size_t n = 0, cap = 0, line_no = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line_no++;
        cJSON *j = cJSON_Parse(line);
        if (!j) continue; /* torn line after a crash */
        cJSON *id = cJSON_GetObjectItem(j, "id"), *content = cJSON_GetObjectItem(j, "content"),
              *status = cJSON_GetObjectItem(j, "status");
        if (!cJSON_IsString(id) || !valid_id(id->valuestring) || !cJSON_IsString(content) || !cJSON_IsString(status)) {
            cJSON_Delete(j);
            continue;
        }
        size_t k = 0;
        while (k < n && strcmp(slots[k].r.id, id->valuestring)) k++;
        if (k == n) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                Slot *s = realloc(slots, cap * sizeof(Slot));
                if (!s) {
                    cJSON_Delete(j);
                    break;
                }
                slots = s;
            }
            memset(&slots[n], 0, sizeof(Slot));
            snprintf(slots[n].r.id, sizeof(slots[n].r.id), "%s", id->valuestring);
            n++;
        }
        Record *r = &slots[k].r;
        RecordVersion *vs = realloc(r->versions, (r->nversions + 1) * sizeof(RecordVersion));
        if (!vs) {
            cJSON_Delete(j);
            break;
        }
        r->versions = vs;
        RecordVersion *v = &vs[r->nversions];
        memset(v, 0, sizeof(*v));
        double ver = num(j, "ver");
        v->ver = ver < 1e9 ? (int)ver : 0;
        v->ts = (time_t)num(j, "ts");
        snprintf(v->status, sizeof(v->status), "%s", status->valuestring);
        v->content = strdup(content->valuestring);
        cJSON *srcs = cJSON_GetObjectItem(j, "sources");
        int ns = cJSON_IsArray(srcs) ? cJSON_GetArraySize(srcs) : 0;
        if (ns > 0) v->sources = calloc((size_t)ns, sizeof(SourceRef));
        for (int i = 0; v->sources && i < ns; i++)
            if (parse_source(cJSON_GetArrayItem(srcs, i), &v->sources[v->nsources])) v->nsources++;
        if (v->content) r->nversions++;
        else records_free_sources(v->sources, v->nsources);
        slots[k].last_line = line_no;
        cJSON_Delete(j);
    }
    free(text);
    qsort(slots, n, sizeof(Slot), by_recent);
    Record *list = n ? malloc(n * sizeof(Record)) : NULL;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (list && slots[i].r.nversions) list[m++] = slots[i].r;
        else free_record(&slots[i].r);
    }
    free(slots);
    *out = list;
    return m;
}

bool records_append(const char *project_dir, RecordKind kind, const char *id, const char *status, const char *content,
                    const SourceRef *sources, size_t nsources, int *ver_out)
{
    if (!valid_id(id)) return false;
    char path[1200];
    if (!log_path(project_dir, kind, true, path, sizeof(path))) return false;

    /* Next version number: one past the newest stored. */
    int ver = 1;
    Record *list = NULL;
    size_t n = records_list(project_dir, kind, &list);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(list[i].id, id)) ver = record_current(&list[i])->ver + 1;
    records_free(list, n);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", id);
    cJSON_AddNumberToObject(j, "ver", ver);
    cJSON_AddNumberToObject(j, "ts", (double)time(NULL));
    cJSON_AddStringToObject(j, "status", status);
    cJSON_AddStringToObject(j, "content", content ? content : "");
    cJSON *arr = cJSON_AddArrayToObject(j, "sources");
    for (size_t i = 0; i < nsources; i++) cJSON_AddItemToArray(arr, source_json(&sources[i]));
    char *s = cJSON_PrintUnformatted(j);
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
