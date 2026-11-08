/* Measures the legacy-project import that runs synchronously on first open
 * (MEMORY-019 phase 1): wall time, number of SQL statements the server saw,
 * and rows created, for a project folder seeded by tests/seed_manuscript.py.
 * Diagnostic only; disposable database only.
 *
 *   import_probe DATABASE PROJECT_DIR
 */
#define _DARWIN_C_SOURCE
#include "app/db.h"
#include "app/store.h"
#include "app/store_backends.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static long long status(Db *db, const char *name)
{
    DbRows r;
    char sql[128];
    snprintf(sql, sizeof(sql), "SHOW GLOBAL STATUS LIKE '%s'", name);
    if (!db_query(db, sql, NULL, 0, &r) || r.nrows != 1) return -1;
    long long value = atoll(db_cell(&r, 0, 1));
    db_rows_free(&r);
    return value;
}

static long long count(Db *db, const char *table)
{
    DbRows r;
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    if (!db_query(db, sql, NULL, 0, &r) || r.nrows != 1) return -1;
    long long value = atoll(db_cell(&r, 0, 0));
    db_rows_free(&r);
    return value;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: import_probe DATABASE PROJECT_DIR\n"); return 2; }
    StoreManager *store = store_manager_create();
    char why[512];
    if (!store || !store_start(store, argv[1], why, sizeof(why))) { fprintf(stderr, "store: %s\n", why); return 1; }
    Db *db = store_db(store);
    if (!db) { fprintf(stderr, "no connection\n"); return 1; }
    long long questions0 = status(db, "Questions"), commits0 = status(db, "Com_commit"), inserts0 = status(db, "Com_insert");
    double t0 = now();
    bool ok = store_import(store, argv[2], why, sizeof(why));
    double seconds = now() - t0;
    long long questions1 = status(db, "Questions"), commits1 = status(db, "Com_commit"), inserts1 = status(db, "Com_insert");
    printf("IMPORT ok=%d seconds=%.3f statements=%lld inserts=%lld commits=%lld", ok, seconds,
           questions1 - questions0, inserts1 - inserts0, commits1 - commits0);
    if (!ok) printf(" why=\"%s\"", why);
    printf("\nROWS messages=%lld conversations=%lld documents=%lld revisions=%lld records=%lld\n",
           count(db, "messages"), count(db, "conversations"), count(db, "documents"), count(db, "revisions"), count(db, "records"));
    store_manager_destroy(store);
    return ok ? 0 : 1;
}
