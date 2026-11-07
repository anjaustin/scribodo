/* DB-only scale diagnostic. Runner owns the disposable database lifecycle.
 * No app, worker, model, UI, file adapter, or author's project is exercised.
 * Run tools/history_bench.sh --help. Latencies are observations, not an SLA. */
#define _DARWIN_C_SOURCE
#include "app/db.h"
#include "app/store_backends.h"
#include "core/utf8.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static Db *db;
static const char *project = "p_history_bench";
static const size_t oversized = 262147;

static void fail(const char *what)
{
    fprintf(stderr, "history_bench: %s%s%s\n", what, db ? ": " : "", db ? db_error(db) : "");
    if (db) { db_rollback(db); db_close(db); }
    exit(1);
}

#define REQUIRE(x) do { if (!(x)) fail(#x); } while (0)

static unsigned number(const char *s, unsigned max)
{
    if (!s || !*s || *s == '0') return 0;
    unsigned n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > max / 10) return 0;
        n = n * 10 + (unsigned)(*s - '0');
        if (n > max) return 0;
    }
    return n;
}

static bool safe_name(const char *s)
{
    const char *prefix = "scribodo_test_history_";
    if (strncmp(s, prefix, strlen(prefix))) return false;
    s += strlen(prefix);
    size_t digits = strspn(s, "0123456789");
    if (!digits || digits > 10 || s[digits] != '_') return false;
    s += digits + 1;
    return strlen(s) == 16 && strspn(s, "0123456789abcdef") == 16;
}

/* Deterministic multilingual prose, newlines, quotes and SQL-like punctuation.
 * Fill the final few bytes with ASCII rather than splitting a codepoint. */
static char *body(size_t bytes, size_t seq)
{
    const char *prose[] = {
        "Chapter: the rain crossed the harbour. 'Revise this'; -- ?\n",
        "Élodie — café, naïve; é. 東京の夜。\n",
        "حكاية المساء — Привет, мир! 🌫️📚\n"
    };
    char *s = malloc(bytes + 1);
    REQUIRE(s);
    size_t at = 0;
    while (at < bytes) {
        const char *p = prose[(seq + at) % 3];
        size_t n = strlen(p);
        if (n > bytes - at) { memset(s + at, 'x', bytes - at); break; }
        memcpy(s + at, p, n); at += n;
    }
    s[bytes] = 0;
    return s;
}

static size_t body_size(size_t n, size_t seq)
{
    static const size_t sizes[] = {64, 256, 1024, 4096, 8192};
    return seq == n / 2 ? oversized : sizes[seq % 5];
}

static double now_ms(void)
{
    struct timespec ts;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec * 1000 + (double)ts.tv_nsec / 1e6;
}

static double rss_mib(void)
{
    struct rusage r;
    REQUIRE(getrusage(RUSAGE_SELF, &r) == 0);
#ifdef __APPLE__
    return (double)r.ru_maxrss / (1024 * 1024);
#else
    return (double)r.ru_maxrss / 1024;
#endif
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile(const double *v, unsigned count, unsigned percent)
{
    return v[((size_t)count * percent + 99) / 100 - 1]; /* nearest rank */
}

static void seed(const char *cid, size_t n)
{
    REQUIRE(db_begin(db));
    REQUIRE(db_run(db, "INSERT INTO conversations(id,project_id,title,created_at,updated_at) VALUES(?,?,?,?,?)",
                   (DbArg[]){DB_S(cid), DB_S(project), DB_S("Manuscript archive"), DB_D(1), DB_D(1)}, 5, NULL));
    uint64_t bytes = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = body_size(n, i);
        char *text = body(len, i);
        bool ok = db_run(db, "INSERT INTO messages(conversation_id,seq,role,content,error,is_edit,truncated,created_at,via,stopped)"
                            " VALUES(?,?,?,?,?,?,?,?,?,?)",
                         (DbArg[]){DB_S(cid), DB_I(i), DB_S(i % 2 ? "assistant" : "user"), DB_S(text),
                                   DB_I(i % 97 == 0), DB_I(i % 79 == 0), DB_I(i % 113 == 0), DB_D(i + 1),
                                   DB_S(i % 3 ? NULL : "bench/provider"), DB_I(i % 127 == 0)}, 10, NULL);
        free(text);
        REQUIRE(ok);
        bytes += len;
    }
    REQUIRE(db_commit(db));
    printf("archive messages=%zu body_bytes=%" PRIu64 " oversized_seq=%zu oversized_bytes=%zu rss_highwater_mib=%.2f\n",
           n, bytes, n / 2, oversized, rss_mib());
}

static void check_window(const ChatWindowPage *p, const ChatWindowRequest *r, size_t n)
{
    REQUIRE(p->count > 0 && p->count <= CHAT_SOURCE_PAGE_ROWS);
    REQUIRE(p->text_bytes <= CHAT_SOURCE_PAGE_BYTES);
    size_t bytes = 0;
    for (size_t i = 0; i < p->count; i++) {
        const ChatMessage *m = &p->messages[i];
        REQUIRE(m->sequence_known && m->sequence >= 0 && (uint64_t)m->sequence < n);
        REQUIRE(!i || m->sequence > p->messages[i - 1].sequence);
        REQUIRE(m->content && strlen(m->content) <= CHAT_SOURCE_CHUNK_BYTES);
        size_t len = strlen(m->content), invalid = 0;
        uint32_t decoded[CHAT_SOURCE_CHUNK_BYTES];
        utf8_decode((const uint8_t *)m->content, len, decoded, &invalid);
        REQUIRE(!invalid);
        size_t total = body_size(n, (size_t)m->sequence), offset = 0;
        char *expected = body(total, (size_t)m->sequence);
        if (r->direction == CHAT_WINDOW_FOCUS && m->sequence == r->sequence) {
            for (size_t cp = 0; cp < r->start; cp++) {
                uint32_t value;
                REQUIRE(offset < total);
                offset += (size_t)utf8_decode_one((const uint8_t *)expected + offset, total - offset, &value);
            }
        }
        REQUIRE(len <= total - offset && !memcmp(expected + offset, m->content, len));
        REQUIRE(m->excerpt == (offset != 0 || len < total));
        REQUIRE(m->excerpt_more == (len < total - offset));
        free(expected);
        if (r->direction == CHAT_WINDOW_BEFORE) REQUIRE(m->sequence < r->sequence);
        if (r->direction == CHAT_WINDOW_FOCUS) REQUIRE(m->sequence <= r->sequence);
        bytes += strlen(m->content);
    }
    REQUIRE(bytes == p->text_bytes);
    const ChatMessage *last = &p->messages[p->count - 1];
    if (r->direction == CHAT_WINDOW_LATEST) REQUIRE(last->sequence == (int64_t)n - 1 && !p->newer);
    if (r->direction == CHAT_WINDOW_FOCUS) {
        REQUIRE(last->sequence == r->sequence && last->excerpt && last->excerpt_more && last->excerpt_start == r->start);
    }
    REQUIRE(p->older == (p->messages[0].sequence > 0));
    REQUIRE(p->newer == (last->sequence < (int64_t)n - 1));
}

/* Validate a full oversized source, byte-for-byte, across UTF-8 boundaries.
 * Isolated cursor bounds exclude the rest of the archive deliberately. */
static void source_reconstruction(const char *cid, size_t n)
{
    ChatSourceCursor cursor = {.started = true, .seq = (int64_t)n / 2, .end_seq = (int64_t)n / 2};
    char *expected = body(oversized, n / 2);
    size_t copied = 0, pages = 0;
    bool more;
    do {
        ChatSourcePage p;
        REQUIRE(sdb_chat_source_page(db, project, cid, &cursor, &p));
        REQUIRE(p.count && p.count <= CHAT_SOURCE_PAGE_ROWS && p.text_bytes <= CHAT_SOURCE_PAGE_BYTES);
        size_t bytes = 0;
        for (size_t i = 0; i < p.count; i++) {
            const ChatSourceChunk *c = &p.chunks[i];
            size_t len = strlen(c->text);
            REQUIRE(len && len <= CHAT_SOURCE_CHUNK_BYTES && c->seq == (int64_t)n / 2);
            REQUIRE(c->byte_offset == copied && c->total_bytes == oversized && len <= oversized - copied);
            REQUIRE(!memcmp(expected + copied, c->text, len));
            copied += len; bytes += len;
        }
        REQUIRE(bytes == p.text_bytes && ++pages <= oversized / CHAT_SOURCE_CHUNK_BYTES + 2);
        cursor = p.next; more = p.has_more;
        chat_source_page_free(&p);
    } while (more);
    REQUIRE(copied == oversized);
    free(expected);
    printf("source_reconstruction messages=%zu pages=%zu bytes=%zu PASS\n", n, pages, copied);
}

static void measure(const char *cid, size_t n, unsigned iterations, const char *kind)
{
    ChatWindowRequest r = {0};
    bool source = !strcmp(kind, "source_page");
    if (!strcmp(kind, "older")) r = (ChatWindowRequest){.direction = CHAT_WINDOW_BEFORE, .sequence = (int64_t)n / 2};
    if (!strcmp(kind, "focus")) r = (ChatWindowRequest){.direction = CHAT_WINDOW_FOCUS, .sequence = (int64_t)n / 2, .start = 10003};
    double *samples = calloc(iterations, sizeof(*samples));
    REQUIRE(samples);
    size_t min_rows = SIZE_MAX, max_rows = 0, min_bytes = SIZE_MAX, max_bytes = 0;
    for (unsigned i = 0; i < iterations + 3; i++) {
        size_t rows, bytes;
        double start = now_ms(), elapsed;
        if (source) {
            ChatSourcePage p;
            ChatSourceCursor c = {.started = true, .seq = (int64_t)n / 2, .end_seq = (int64_t)n / 2};
            REQUIRE(sdb_chat_source_page(db, project, cid, &c, &p));
            elapsed = now_ms() - start;
            rows = p.count; bytes = p.text_bytes;
            REQUIRE(rows && rows <= CHAT_SOURCE_PAGE_ROWS && bytes <= CHAT_SOURCE_PAGE_BYTES && p.has_more);
            size_t actual = 0;
            for (size_t j = 0; j < rows; j++) {
                size_t len = strlen(p.chunks[j].text);
                REQUIRE(len && len <= CHAT_SOURCE_CHUNK_BYTES);
                actual += len;
            }
            REQUIRE(actual == bytes);
            chat_source_page_free(&p);
        } else {
            ChatWindowPage p;
            REQUIRE(sdb_chat_window(db, project, cid, &r, &p));
            elapsed = now_ms() - start;
            check_window(&p, &r, n);
            rows = p.count; bytes = p.text_bytes;
            chat_window_page_free(&p);
        }
        if (i < 3) continue;
        samples[i - 3] = elapsed;
        if (rows < min_rows) min_rows = rows;
        if (rows > max_rows) max_rows = rows;
        if (bytes < min_bytes) min_bytes = bytes;
        if (bytes > max_bytes) max_bytes = bytes;
    }
    qsort(samples, iterations, sizeof(*samples), compare);
    printf("window messages=%zu kind=%s samples=%u p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f rows=%zu..%zu body_bytes=%zu..%zu rss_highwater_mib=%.2f\n",
           n, kind, iterations, percentile(samples, iterations, 50), percentile(samples, iterations, 95),
           percentile(samples, iterations, 99), min_rows, max_rows, min_bytes, max_bytes, rss_mib());
    fflush(stdout);
    free(samples);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 4 || !safe_name(argv[1]) || (strcmp(argv[2], "quick") && strcmp(argv[2], "soak")) || !number(argv[3], 100000)) {
        fprintf(stderr, "Use tools/history_bench.sh [quick|soak] [--iterations N]; database must be runner-generated.\n");
        return 2;
    }
    const char *port = getenv("SCRIBODO_DB_PORT");
    if (port && !number(port, 65535)) { fprintf(stderr, "invalid SCRIBODO_DB_PORT\n"); return 2; }
    unsigned iterations = number(argv[3], 100000);
    DbConfig config;
    char err[512] = {0};
    if (!db_config_default(&config, err, sizeof(err))) { fprintf(stderr, "%s\n", err); return 1; }
    snprintf(config.database, sizeof(config.database), "%s", argv[1]);
    db = db_open(&config, err, sizeof(err));
    if (!db) { fprintf(stderr, "%s\n", err); return 1; }
    /* Refuse even a benchmark-shaped DB if it contains any existing tables. */
    DbRows tables;
    REQUIRE(db_query(db, "SELECT table_name FROM information_schema.tables WHERE table_schema=? LIMIT 1",
                     (DbArg[]){DB_S(argv[1])}, 1, &tables));
    bool empty = tables.nrows == 0;
    db_rows_free(&tables);
    REQUIRE(empty);
    DbRows version;
    REQUIRE(db_query(db, "SELECT VERSION()", NULL, 0, &version));
    REQUIRE(version.nrows == 1 && db_rows_has_columns(&version, 1));
    printf("server=%s endpoint=%s:%u\n", db_cell(&version, 0, 0), config.host, config.port);
    db_rows_free(&version);
    if (!db_migrate(db, err, sizeof(err))) { fprintf(stderr, "%s\n", err); fail("migrate"); }
    REQUIRE(db_run(db, "INSERT INTO projects(id,name,path,path_hash,created_at,updated_at) VALUES(?,?,?,?,?,?)",
                   (DbArg[]){DB_S(project), DB_S("Disposable benchmark"), DB_S("/not-a-real-project/history-bench"),
                             DB_S("history-bench-only"), DB_D(1), DB_D(1)}, 6, NULL));
    printf("history_bench mode=%s iterations=%u warmups=3 database=%s\n", argv[2], iterations, argv[1]);
    puts("Timing: warm DB API call only (includes SQL/client allocation, excludes validation/free/seed); nearest-rank percentiles, no SLA gate.");
    puts("Bytes: returned body payload only, not SQL wire traffic/metadata. RSS: cumulative whole-client platform high-water, includes migration/seed; not API-isolated or server RSS.");
    puts("Synthetic prose/Unicode 64..8192-byte turns plus one 262147-byte source per archive; no attachments, model, worker or UI. Small-sample p99 may equal maximum.");
    const size_t sizes[] = {100, 1000, 10000};
    const char *kinds[] = {"latest", "older", "focus", "source_page"};
    for (size_t i = 0; i < 3; i++) {
        char cid[40];
        snprintf(cid, sizeof(cid), "bench_%zu", sizes[i]);
        seed(cid, sizes[i]);
        source_reconstruction(cid, sizes[i]);
        for (size_t j = 0; j < 4; j++) measure(cid, sizes[i], iterations, kinds[j]);
    }
    db_close(db); db = NULL;
    puts("PASS: boundedness/source gates; performance diagnostic only.");
    return 0;
}
