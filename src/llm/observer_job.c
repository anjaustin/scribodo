#define _DARWIN_C_SOURCE
#include "observer_job_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app/chat_store.h"
#include "app/project_store.h"
#include "llm/observer.h"

#define CHUNK_MAX 3000
#define PIECE_MAX 2000
#define MAX_DOC_BYTES (16u << 20)

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}

static bool valid_utf8(const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        unsigned char c = u[i];
        size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!k || i + k > n || (k == 2 && c < 0xC2)) return false;
        for (size_t j = 1; j < k; j++) if ((u[i + j] & 0xC0) != 0x80) return false;
        i += k;
    }
    return true;
}

/* ---- state ------------------------------------------------------------------------ */

cJSON *observer_job_state_load(StoreManager *store, const char *project)
{
    return project_store_observer_state_load(store, project);
}

bool observer_job_state_save(StoreManager *store, const char *project, cJSON *state)
{
    return project_store_observer_state_save(store, project, state);
}

static bool hash_seen(cJSON *arr, uint64_t h)
{
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    cJSON *it;
    cJSON_ArrayForEach(it, arr)
    if (cJSON_IsString(it) && !strcmp(it->valuestring, hex)) return true;
    return false;
}

/* ---- jobs ------------------------------------------------------------------------- */

void observer_job_free(ObserverJob *j)
{
    free(j->src);
    for (size_t i = 0; i < j->nmsgs; i++) free(j->msgs[i]);
    free(j->msgs);
    free(j->chunk);
    free(j->segs);
    free(j->hashes);
    cJSON_Delete(j->current);
    memset(j, 0, sizeof(*j));
}

/* Add a piece (at most PIECE_MAX bytes) to the chunk; false if it doesn't fit. */
static bool chunk_add(ObserverJob *j, const char *label, const char *text, size_t src_off, size_t len, long message)
{
    size_t ll = label ? strlen(label) : 0;
    if (j->chunk_len && j->chunk_len + 2 + ll + len > CHUNK_MAX) return false;
    if (j->nsegs == j->cap_segs) {
        size_t cap = j->cap_segs ? j->cap_segs * 2 : 16;
        ObserverSegment *sg = realloc(j->segs, cap * sizeof(ObserverSegment));
        if (!sg) return false;
        j->segs = sg;
        j->cap_segs = cap;
    }
    char *c = realloc(j->chunk, j->chunk_len + ll + len + 3);
    if (!c) return false;
    j->chunk = c;
    if (j->chunk_len) {
        memcpy(j->chunk + j->chunk_len, "\n\n", 2);
        j->chunk_len += 2;
    }
    if (ll) memcpy(j->chunk + j->chunk_len, label, ll);
    j->chunk_len += ll;
    j->segs[j->nsegs++] = (ObserverSegment){ j->chunk_len, src_off, len, message };
    memcpy(j->chunk + j->chunk_len, text + src_off, len);
    j->chunk_len += len;
    j->chunk[j->chunk_len] = '\0';
    return true;
}

static bool add_hash(ObserverJob *j, uint64_t h)
{
    if (j->nhashes == j->cap_hashes) {
        size_t cap = j->cap_hashes ? j->cap_hashes * 2 : 32;
        uint64_t *hs = realloc(j->hashes, cap * sizeof(uint64_t));
        if (!hs) return false;
        j->hashes = hs;
        j->cap_hashes = cap;
    }
    j->hashes[j->nhashes++] = h;
    return true;
}

/* Length of the next piece of s[0, n): at most PIECE_MAX bytes, ending at a
 * sentence end or a space when possible, never inside a UTF-8 sequence. Long
 * paragraphs and messages are read piece by piece, each tracked on its own
 * (red-team 005: everything past 3,000 bytes used to be marked read unread). */
static size_t piece_len(const char *s, size_t n)
{
    if (n <= PIECE_MAX) return n;
    size_t cut = 0;
    for (size_t i = PIECE_MAX; i > PIECE_MAX / 2 && !cut; i--)
        if ((s[i - 1] == '.' || s[i - 1] == '?' || s[i - 1] == '!' || s[i - 1] == '\n') && s[i] == ' ') cut = i;
    for (size_t i = PIECE_MAX; i > PIECE_MAX / 2 && !cut; i--)
        if (s[i] == ' ') cut = i;
    if (!cut) {
        cut = PIECE_MAX;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    }
    return cut;
}

/* Read a document as the editor does (doc_io): drop a UTF-8 BOM, turn CRLF
 * and lone CR into LF, so offsets match what the author sees and what a
 * save writes (red-team 005: CRLF files had every offset shifted). */
static char *read_doc(const char *path, size_t *n)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > MAX_DOC_BYTES) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc((size_t)st.st_size + 1);
    size_t got = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    size_t r = 0, w = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) r = 3;
    for (; r < got; r++) {
        if (buf[r] == '\r') {
            if (r + 1 < got && buf[r + 1] == '\n') r++;
            buf[w++] = '\n';
        } else {
            buf[w++] = buf[r];
        }
    }
    buf[w] = '\0';
    *n = w;
    return buf;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void observer_job_set_chat_progress(cJSON *chats, const char *id, size_t n, size_t off)
{
    cJSON_DeleteItemFromObject(chats, id);
    cJSON *o = cJSON_AddObjectToObject(chats, id);
    cJSON_AddNumberToObject(o, "n", (double)n);
    cJSON_AddNumberToObject(o, "off", (double)off);
}

static bool in_list(char **names, size_t n, const char *s)
{
    for (size_t i = 0; i < n; i++)
        if (names[i] && !strcmp(names[i], s)) return true;
    return false;
}

/* The next unread piece of stored text, or false if all is read. Also
 * prunes state for paragraphs, documents and chats that no longer exist. */
bool observer_job_find(StoreManager *store, const char *project, cJSON *st, ObserverJob *j)
{
    memset(j, 0, sizeof(*j));
    cJSON *docs = cJSON_GetObjectItem(st, "docs"), *chats = cJSON_GetObjectItem(st, "chats");
    /* Documents, by name (no cap). */
    DIR *d = opendir(project);
    char **names = NULL;
    size_t nn = 0, cap = 0;
    struct dirent *ent;
    while (d && (ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (ent->d_name[0] == '.') continue;
        if (!((n > 3 && !strcasecmp(ent->d_name + n - 3, ".md")) || (n > 4 && !strcasecmp(ent->d_name + n - 4, ".txt")) ||
              (n > 9 && !strcasecmp(ent->d_name + n - 9, ".markdown"))))
            continue;
        if (nn == cap) {
            cap = cap ? cap * 2 : 64;
            char **nm = realloc(names, cap * sizeof(char *));
            if (!nm) break;
            names = nm;
        }
        names[nn++] = strdup(ent->d_name);
    }
    if (d) closedir(d);
    if (nn) qsort(names, nn, sizeof(char *), cmp_names);
    /* Forget documents that are gone. */
    for (cJSON *it = docs ? docs->child : NULL, *next; it; it = next) {
        next = it->next;
        if (!in_list(names, nn, it->string)) cJSON_DeleteItemFromObject(docs, it->string);
    }
    bool found = false;
    for (size_t i = 0; i < nn && !found; i++) {
        if (!names[i]) continue;
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", project, names[i]);
        size_t len = 0;
        char *text = read_doc(path, &len);
        if (!text || strlen(text) != len || !valid_utf8(text, len)) { /* not a readable UTF-8 text file */
            free(text);
            continue;
        }
        cJSON *seen = cJSON_GetObjectItem(docs, names[i]);
        cJSON *kept = cJSON_CreateArray(); /* seen hashes still present (pruned) */
        size_t np = 0, pcap = 256;
        size_t *ps = malloc(pcap * sizeof(size_t)), *pl = malloc(pcap * sizeof(size_t));
        while (ps && pl) { /* all paragraphs, however many */
            np = obs_paragraphs(text, ps, pl, pcap);
            if (np < pcap) break;
            pcap *= 2;
            size_t *a = realloc(ps, pcap * sizeof(size_t)), *b = realloc(pl, pcap * sizeof(size_t));
            if (!a || !b) {
                free(a ? a : ps);
                free(b ? b : pl);
                ps = pl = NULL;
                break;
            }
            ps = a;
            pl = b;
        }
        bool full = false;
        for (size_t p = 0; ps && pl && p < np; p++) {
            for (size_t off = 0; off < pl[p];) {
                size_t plen = piece_len(text + ps[p] + off, pl[p] - off);
                while (off + plen < pl[p] && text[ps[p] + off + plen] == ' ') plen++; /* keep spaces with the piece */
                uint64_t h = fnv(text + ps[p] + off, plen);
                if (seen && hash_seen(seen, h)) {
                    char hex[17];
                    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
                    if (!hash_seen(kept, h)) cJSON_AddItemToArray(kept, cJSON_CreateString(hex));
                } else if (!full) {
                    if (chunk_add(j, NULL, text, ps[p] + off, plen, -1)) add_hash(j, h);
                    else full = true;
                }
                off += plen;
            }
        }
        free(ps);
        free(pl);
        if (j->nsegs) {
            /* Every piece the document has now: what was read before plus
             * this job's (the semantic index drops the rest). */
            j->current = cJSON_Duplicate(kept, true);
            for (size_t h = 0; j->current && h < j->nhashes; h++) {
                char hex[17];
                snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)j->hashes[h]);
                cJSON_AddItemToArray(j->current, cJSON_CreateString(hex));
            }
        }
        cJSON_DeleteItemFromObject(docs, names[i]);
        cJSON_AddItemToObject(docs, names[i], kept);
        if (j->nsegs) {
            j->is_doc = true;
            snprintf(j->name, sizeof(j->name), "%s", names[i]);
            snprintf(j->path, sizeof(j->path), "%s", path);
            j->src = text;
            found = true;
        } else {
            free(text);
        }
    }
    for (size_t i = 0; i < nn; i++) free(names[i]);
    free(names);
    if (found) return true;

    /* Chats: what follows the position already read. */
    ChatSummary *list = NULL;
    size_t nc = chat_store_list(store, project, &list);
    for (cJSON *it = chats ? chats->child : NULL, *next; it; it = next) { /* forget deleted chats */
        next = it->next;
        bool present = false;
        for (size_t i = 0; i < nc && !present; i++) present = !strcmp(list[i].id, it->string);
        if (!present) cJSON_DeleteItemFromObject(chats, it->string);
    }
    for (size_t i = 0; i < nc && !found; i++) {
        if (list[i].observer_ignore) continue; /* the author said not to read it */
        ChatMessage *msgs = NULL;
        size_t n = 0;
        if (!chat_store_load(store, project, list[i].id, &msgs, &n)) continue;
        cJSON *pos = cJSON_GetObjectItem(chats, list[i].id);
        size_t from = 0, from_off = 0;
        if (cJSON_IsNumber(pos) && pos->valuedouble >= 0) from = (size_t)pos->valuedouble; /* older state */
        if (cJSON_IsObject(pos)) {
            cJSON *a = cJSON_GetObjectItem(pos, "n"), *b = cJSON_GetObjectItem(pos, "off");
            if (cJSON_IsNumber(a) && a->valuedouble >= 0) from = (size_t)a->valuedouble;
            if (cJSON_IsNumber(b) && b->valuedouble >= 0) from_off = (size_t)b->valuedouble;
        }
        if (from < n) {
            j->msgs = calloc(n, sizeof(char *));
            j->nmsgs = j->msgs ? n : 0;
            for (size_t k = 0; k < j->nmsgs; k++) j->msgs[k] = strdup(msgs[k].content ? msgs[k].content : "");
            size_t k = from, off = from_off;
            bool full = false;
            for (; k < j->nmsgs && !full; k++, off = 0) {
                /* Failed replies, and AI rewrites (which may have been
                 * rejected), are not story text. */
                bool skip = msgs[k].error || !j->msgs[k] || !j->msgs[k][0] || (msgs[k].role == CHAT_ASSISTANT && msgs[k].is_edit);
                if (skip) continue;
                const char *label = msgs[k].role == CHAT_USER ? "AUTHOR: " : "ASSISTANT: ";
                size_t mlen = strlen(j->msgs[k]);
                if (off > mlen) off = mlen;
                while (off < mlen) {
                    size_t plen = piece_len(j->msgs[k] + off, mlen - off);
                    if (!chunk_add(j, off == 0 ? label : NULL, j->msgs[k], off, plen, (long)k)) {
                        full = true;
                        break;
                    }
                    off += plen;
                }
                if (full) break;
            }
            j->msg_to = k;
            j->off_to = full ? off : 0;
            snprintf(j->name, sizeof(j->name), "%s", list[i].id);
            found = true;
            if (!j->nsegs) { /* only failed/empty/rewrite messages: just mark them read */
                observer_job_set_chat_progress(chats, list[i].id, j->msg_to, j->off_to);
                observer_job_free(j);
                found = false;
            }
        }
        chat_store_free_messages(msgs, n);
    }
    free(list);
    return found;
}
