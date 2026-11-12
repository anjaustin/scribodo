/* GAP-004 interruption matrix: the process dies at every named point of a transition and the next process
 * must find the row's invariant. Rows here: the schema migration (after each statement, after each receipt)
 * and the document save (after the temp file is complete, after the rename).
 *
 * The driver forks and execs itself with SCRIBODO_CRASH_POINT="<point>#k" for k = 1, 2, ... on a fresh
 * throwaway database each time, until a child reports it finished without dying (the matrix is exhausted:
 * k exceeded the number of hits). For every death it checks the child died of SIGKILL, reads the schema
 * version the dead process left, runs db_migrate in this process, and compares the schema (every column,
 * index and constraint from information_schema) with a reference database migrated once without
 * interruption. One row per point and hit goes to build/crash/matrix.tsv.
 *
 * Limits: process death, not power loss; the database server stays up, so what it has committed is what a
 * durable server would have committed, and the filesystem keeps what the dead process had written. Rows
 * seeded before the migration are the replay test's job (tests/test_db_core.c); this binary holds the schema. */
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>

#include "app/db.h"
#include "core/crash_point.h"
#include "core/doc_io.h"
#include "core/utf8.h"
#include "test_util.h"

bool admin_name(const char *sql_fmt, const char *name);

#define LATEST_SCHEMA 15

static const char *const k_points[] = { "migration.statement", "migration.receipt" };

static Db *open_named(const char *name, char *err, size_t errlen)
{
    DbConfig c;
    if (!db_config_default(&c, err, errlen)) return NULL;
    snprintf(c.database, sizeof(c.database), "%s", name);
    return db_open(&c, err, errlen);
}

/* Every column, index and constraint of the database, one line each, in a fixed order. */
static char *schema_fingerprint(Db *db, const char *name)
{
    static const char *const queries[] = {
        "SELECT CONCAT_WS('|', 'column', table_name, column_name, ordinal_position, column_type, is_nullable, "
        "COALESCE(column_default, 'NULL'), column_key, extra, COALESCE(character_set_name, ''), COALESCE(collation_name, '')) "
        "FROM information_schema.columns WHERE table_schema = ? ORDER BY table_name, ordinal_position",
        "SELECT CONCAT_WS('|', 'index', table_name, index_name, seq_in_index, column_name, non_unique, index_type, COALESCE(sub_part, 0)) "
        "FROM information_schema.statistics WHERE table_schema = ? ORDER BY table_name, index_name, seq_in_index",
        "SELECT CONCAT_WS('|', 'constraint', table_name, constraint_name, constraint_type) "
        "FROM information_schema.table_constraints WHERE table_schema = ? ORDER BY table_name, constraint_name",
        "SELECT CONCAT_WS('|', 'check', table_name, constraint_name, check_clause) "
        "FROM information_schema.check_constraints WHERE constraint_schema = ? ORDER BY table_name, constraint_name",
        "SELECT CONCAT_WS('|', 'table', table_name, engine, table_collation) "
        "FROM information_schema.tables WHERE table_schema = ? ORDER BY table_name",
    };
    size_t cap = 65536, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        DbRows r;
        if (!db_query(db, queries[q], (DbArg[1]){ DB_S(name) }, 1, &r)) { free(out); return NULL; }
        for (size_t i = 0; i < r.nrows; i++) {
            const char *cell = db_cell(&r, i, 0);
            size_t need = len + strlen(cell) + 2;
            if (need > cap) {
                char *grown = realloc(out, need * 2);
                if (!grown) { db_rows_free(&r); free(out); return NULL; }
                out = grown; cap = need * 2;
            }
            len += (size_t)snprintf(out + len, cap - len, "%s\n", cell);
        }
        db_rows_free(&r);
    }
    return out;
}

static int child_main(const char *name)
{
    char err[512] = "";
    Db *db = open_named(name, err, sizeof(err));
    if (!db) { fprintf(stderr, "child: %s\n", err); return 2; }
    bool ok = db_migrate(db, err, sizeof(err));
    if (!ok) fprintf(stderr, "child: %s\n", err);
    printf("DONE ok=%d version=%d hits=%ld\n", ok, db_schema_version(db), crash_point_hits());
    fflush(stdout);
    db_close(db);
    return ok ? 0 : 2;
}

typedef struct { bool exited, killed; int code, sig; long hits; int version; } ChildResult;

/* Runs this binary as a child under one selected point (mode --child or --child-save); its stdout's DONE line is
 * parsed when it finishes. */
static bool run_child(const char *self, const char *spec, const char *mode, const char *name, ChildResult *out)
{
    memset(out, 0, sizeof(*out));
    out->hits = -1; out->version = -1;
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setenv("SCRIBODO_CRASH_POINT", spec, 1);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execl(self, self, mode, name, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char buf[512] = "";
    size_t got = 0;
    for (;;) {
        ssize_t n = read(pipefd[0], buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        if (got >= sizeof(buf) - 1) break;
    }
    close(pipefd[0]);
    buf[got] = '\0';
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    out->exited = WIFEXITED(status); out->code = out->exited ? WEXITSTATUS(status) : -1;
    out->killed = WIFSIGNALED(status); out->sig = out->killed ? WTERMSIG(status) : 0;
    int ok = 0;
    if (out->exited) sscanf(buf, "DONE ok=%d version=%d hits=%ld", &ok, &out->version, &out->hits);
    return true;
}

static FILE *g_matrix;
static const char *g_self;
static char *g_reference;
static int g_rows;

static void run_point(const char *point)
{
    for (long k = 1; k < 10000; k++) {
        char name[64], spec[160], err[512] = "";
        snprintf(name, sizeof(name), "scribodo_crash_%d_%ld", (int)getpid(), k);
        snprintf(spec, sizeof(spec), "%s#%ld", point, k);
        CHECK(admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", name) && admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", name));
        ChildResult r;
        CHECK(run_child(g_self, spec, "--child", name, &r));
        if (r.exited) {
            /* The matrix is exhausted: the child finished every point before hit k. */
            CHECK(r.code == 0 && r.version == LATEST_SCHEMA && r.hits == k - 1);
            fprintf(g_matrix, "%s\t%ld\tnone\t-\t%d\t-\t%ld hits in all\n", point, k, r.version, r.hits);
            printf("%s: %ld points, every one recovered to schema %d\n", point, k - 1, LATEST_SCHEMA);
            admin_name("DROP DATABASE %s", name);
            break;
        }
        CHECK(r.killed && r.sig == SIGKILL);
        Db *db = open_named(name, err, sizeof(err));
        CHECK(db != NULL);
        if (!db) { fprintf(stderr, "    %s\n", err); admin_name("DROP DATABASE %s", name); break; }
        int at_death = db_schema_version(db);
        /* A death after a statement leaves the migration unreceipted (schema below 15); a death after receipt k
         * leaves exactly k receipts, the fifteenth included. */
        if (!strcmp(point, "migration.receipt")) CHECK(at_death == k);
        else CHECK(at_death >= 0 && at_death < LATEST_SCHEMA);
        bool recovered = db_migrate(db, err, sizeof(err));
        if (!recovered) fprintf(stderr, "    %s#%ld: %s\n", point, k, err);
        CHECK(recovered && db_schema_version(db) == LATEST_SCHEMA);
        DbRows rows;
        bool receipts = db_query(db, "SELECT COUNT(*), COUNT(DISTINCT version), MIN(version), MAX(version) FROM schema_migrations", NULL, 0, &rows) &&
                        rows.nrows == 1 && !strcmp(db_cell(&rows, 0, 0), "15") && !strcmp(db_cell(&rows, 0, 1), "15") &&
                        !strcmp(db_cell(&rows, 0, 2), "1") && !strcmp(db_cell(&rows, 0, 3), "15");
        if (rows.nrows) db_rows_free(&rows);
        CHECK(receipts);
        char *fp = schema_fingerprint(db, name);
        bool same = fp && g_reference && !strcmp(fp, g_reference);
        CHECK(same);
        if (!same && fp && g_reference) {
            /* Show the first differing line of each. */
            const char *a = fp, *b = g_reference;
            while (*a && *b && *a == *b) { a++; b++; }
            fprintf(stderr, "    schema differs at: %.120s\n    reference:         %.120s\n", a, b);
        }
        CHECK(db_migrate(db, err, sizeof(err)) && db_schema_version(db) == LATEST_SCHEMA); /* idempotent after recovery */
        fprintf(g_matrix, "%s\t%ld\tSIGKILL\t%d\t%d\t%s\t%s\n", point, k, at_death, db_schema_version(db), receipts ? "15 receipts" : "receipts wrong",
                same ? "schema identical" : "schema differs");
        g_rows++;
        free(fp);
        db_close(db);
        admin_name("DROP DATABASE %s", name);
    }
}

/* ---- the document save row ------------------------------------------------ */

static const char k_original[] = "The original text, saved whole before the crash.\n";
static const char k_replacement[] = "The replacement text; longer than the original so a mix would be visible, and whole.\n";
static const char k_third[] = "A third save after recovery.\n";

static size_t to_cps(const char *text, uint32_t *out, size_t cap)
{
    size_t n = strlen(text);
    if (n > cap) return 0;
    size_t invalid = 0;
    return utf8_decode((const uint8_t *)text, n, out, &invalid);
}

static bool save_text(const char *path, const char *text, char *err, size_t errlen)
{
    uint32_t cps[512];
    size_t n = to_cps(text, cps, sizeof(cps) / sizeof(cps[0]));
    return n && doc_save(path, cps, n, false, false, NULL, NULL, err, errlen) == DOC_OK;
}

static int child_save_main(const char *path)
{
    char err[512] = "";
    bool ok = save_text(path, k_replacement, err, sizeof(err));
    if (!ok) fprintf(stderr, "child: %s\n", err);
    printf("DONE ok=%d version=0 hits=%ld\n", ok, crash_point_hits());
    fflush(stdout);
    return ok ? 0 : 2;
}

static size_t read_whole(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return (size_t)-1;
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

/* Temp files the save leaves beside the target (doc_io names them <target>.scribodo-XXXXXX). */
static int count_temps(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        if (strstr(ent->d_name, ".scribodo-")) n++;
    closedir(d);
    return n;
}

static const char *content_name(const char *buf, size_t n)
{
    if (n == strlen(k_original) && !memcmp(buf, k_original, n)) return "original";
    if (n == strlen(k_replacement) && !memcmp(buf, k_replacement, n)) return "new";
    if (n == strlen(k_third) && !memcmp(buf, k_third, n)) return "third";
    return "OTHER";
}

static void run_save_point(const char *point)
{
    const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    for (long k = 1; k < 100; k++) {
        char dir[1024], path[1100], spec[160], err[512] = "", buf[1024];
        snprintf(dir, sizeof(dir), "%s/scribodo-crash-save-XXXXXX", base);
        CHECK(mkdtemp(dir) != NULL);
        snprintf(path, sizeof(path), "%s/doc.md", dir);
        CHECK(save_text(path, k_original, err, sizeof(err)));
        snprintf(spec, sizeof(spec), "%s#%ld", point, k);
        ChildResult r;
        CHECK(run_child(g_self, spec, "--child-save", path, &r));
        if (r.exited) {
            CHECK(r.code == 0 && r.hits == k - 1);
            fprintf(g_matrix, "%s\t%ld\tnone\t-\t-\t-\t%ld hits in all\n", point, k, r.hits);
            printf("%s: %ld points\n", point, k - 1);
            break;
        }
        CHECK(r.killed && r.sig == SIGKILL);
        size_t n = read_whole(path, buf, sizeof(buf));
        const char *found = n == (size_t)-1 ? "MISSING" : content_name(buf, n);
        /* The target is whole: the original or the new text, never a mix, a truncation or nothing. */
        CHECK(!strcmp(found, "original") || !strcmp(found, "new"));
        int temps_at_death = count_temps(dir);
        /* The next save (the next process) succeeds and the target holds it whole. */
        bool next = save_text(path, k_third, err, sizeof(err));
        if (!next) fprintf(stderr, "    %s#%ld: %s\n", point, k, err);
        CHECK(next);
        n = read_whole(path, buf, sizeof(buf));
        CHECK(n != (size_t)-1 && !strcmp(content_name(buf, n), "third"));
        int temps_after = count_temps(dir);
        fprintf(g_matrix, "%s\t%ld\tSIGKILL\t%s at death\tnext save ok\t%d temp at death, %d after\t%s\n", point, k, found,
                temps_at_death, temps_after, !strcmp(found, "new") || !strcmp(found, "original") ? "target whole" : "TARGET DAMAGED");
        g_rows++;
        /* Leave nothing behind: the temp files are the test's, named by mkstemp under dir. */
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        CHECK(system(cmd) == 0);
    }
}

static void test_save_points(void)
{
    run_save_point("save.temp_written");
    run_save_point("save.renamed");
}

static void test_migration_points(void)
{
    char ref[64], err[512] = "";
    snprintf(ref, sizeof(ref), "scribodo_crash_%d_ref", (int)getpid());
    CHECK(admin_name("CREATE DATABASE %s CHARACTER SET utf8mb4", ref) && admin_name("GRANT ALL ON %s.* TO 'scribodo'@'%%'", ref));
    Db *db = open_named(ref, err, sizeof(err));
    CHECK(db && db_migrate(db, err, sizeof(err)) && db_schema_version(db) == LATEST_SCHEMA);
    if (db) {
        g_reference = schema_fingerprint(db, ref);
        db_close(db);
    }
    CHECK(g_reference && strlen(g_reference) > 1000);
    admin_name("DROP DATABASE %s", ref);
    if (!g_reference) return;
    for (size_t i = 0; i < sizeof(k_points) / sizeof(k_points[0]); i++) run_point(k_points[i]);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--child")) return child_main(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--child-save")) return child_save_main(argv[2]);
    g_self = argv[0];
    mkdir("build", 0755); mkdir("build/crash", 0755);
    g_matrix = fopen("build/crash/matrix.tsv", "w");
    if (!g_matrix) { perror("build/crash/matrix.tsv"); return 1; }
    fprintf(g_matrix, "point\thit\tdeath\tstate_at_death\trecovery\tdetail\tinvariant\n");
    time_t t0 = time(NULL);
    TestCase tests[] = { { "migration_points", test_migration_points }, { "save_points", test_save_points } };
    int rc = run_tests(tests, 2);
    fclose(g_matrix);
    printf("%d death rows in build/crash/matrix.tsv, %ld s\n", g_rows, (long)(time(NULL) - t0));
    free(g_reference);
    return rc;
}
