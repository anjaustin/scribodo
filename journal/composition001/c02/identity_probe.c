/* C02 contract probe. Synthetic file projects only; exit 1 means the desired
 * isolation/compatibility contract is not yet implemented, not a harness crash. */
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app/revisions.h"
#include "app/store.h"
#include "core/file_text.h"

static int checks, failures, setup_errors;
#define REQUIRE(test) do { checks++; if (!(test)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); } } while (0)
#define SETUP(test) do { checks++; if (!(test)) { setup_errors++; fprintf(stderr, "SETUP ERROR line %d: %s\n", __LINE__, #test); return; } } while (0)

static bool join(const char *a, const char *b, char *out, size_t cap)
{
    if (!out || !cap) return false;
    int n = snprintf(out, cap, "%s/%s", a, b);
    if (n > 0 && (size_t)n < cap) return true;
    out[0] = '\0';
    return false;
}

static bool make_project(const char *root, const char *name, char *out, size_t cap)
{
    return join(root, name, out, cap) && mkdir(out, 0755) == 0;
}

static bool write_doc(const char *project, const char *name, const char *text)
{
    char path[1800];
    return join(project, name, path, sizeof(path)) && file_text_write_atomic(path, text, strlen(text), 0644);
}

static void own_history(StoreManager *store, const char *project, const char *name, const char *expected)
{
    Revision *list = NULL;
    size_t n = 0;
    bool ok = revisions_list_checked(store, project, name, &list, &n);
    REQUIRE(ok && n == 1);
    char *text = ok && n ? revisions_load(store, project, name, &list[0]) : NULL;
    REQUIRE(text && !strcmp(text, expected));
    free(text);
    free(list);
}

static void distinct_pair(StoreManager *store, const char *root, const char *label, const char *a, const char *b)
{
    char project[1400];
    SETUP(make_project(root, label, project, sizeof(project)));
    /* Exact manuscript names exist too; the future legacy-ambiguity gate may
     * inspect the project directory rather than infer intent from API inputs. */
    SETUP(write_doc(project, a, "SYNTHETIC A"));
    SETUP(write_doc(project, b, "SYNTHETIC B"));
    bool recorded = false;
    REQUIRE(revisions_record(store, project, a, "SYNTHETIC A", "save", NULL, &recorded) && recorded);
    REQUIRE(revisions_record(store, project, b, "SYNTHETIC B", "save", NULL, &recorded) && recorded);
    own_history(store, project, a, "SYNTHETIC A");
    own_history(store, project, b, "SYNTHETIC B");
    char afile[160] = "", bfile[160] = "";
    REQUIRE(revisions_snapshot(store, project, a, "SYNTHETIC A", afile, sizeof(afile)) && afile[0]);
    REQUIRE(revisions_snapshot(store, project, b, "SYNTHETIC B", bfile, sizeof(bfile)) && bfile[0]);
    Revision ar = {0}, br = {0}; /* SourceRef stores the filename, not the index's text hash. */
    snprintf(ar.file, sizeof(ar.file), "%s", afile);
    snprintf(br.file, sizeof(br.file), "%s", bfile);
    char *at = revisions_load(store, project, a, &ar), *bt = revisions_load(store, project, b, &br);
    REQUIRE(at && !strcmp(at, "SYNTHETIC A"));
    REQUIRE(bt && !strcmp(bt, "SYNTHETIC B"));
    free(at);
    free(bt);
    printf("%s: desired disjoint history/source contract checked\n", label);
}

static bool legacy_history(const char *project, const char *key)
{
    char meta[1500], root[1600], dir[1900], path[2000];
    if (!join(project, ".scribodo", meta, sizeof(meta)) || !join(meta, "revisions", root, sizeof(root)) ||
        !join(root, key, dir, sizeof(dir))) return false;
    if (mkdir(meta, 0755) || mkdir(root, 0755) || mkdir(dir, 0755)) return false;
    const char index[] =
        "{\"file\":\"20261125-000000-000-save.md\",\"origin\":\"save\",\"bytes\":13,\"ts\":1795564800}\n";
    if (!join(dir, "20261125-000000-000-save.md", path, sizeof(path))) return false;
    if (!file_text_write_atomic(path, "LEGACY SOURCE", 13, 0644)) return false;
    if (!join(dir, "index.jsonl", path, sizeof(path))) return false;
    return file_text_write_atomic(path, index, sizeof(index) - 1, 0644);
}

static void legacy_cases(StoreManager *store, const char *root)
{
    char project[1400], prefix[200], a[256], b[256];
    SETUP(make_project(root, "legacy-short", project, sizeof(project)));
    SETUP(legacy_history(project, "chapter.md"));
    int before = failures;
    own_history(store, project, "chapter.md", "LEGACY SOURCE");
    if (failures != before) setup_errors++; /* an expected-red run needs a working legacy control */

    SETUP(make_project(root, "legacy-long-ambiguous", project, sizeof(project)));
    memset(prefix, 'x', 199); prefix[199] = '\0';
    snprintf(a, sizeof(a), "%sA.md", prefix);
    snprintf(b, sizeof(b), "%sB.md", prefix);
    SETUP(write_doc(project, a, "SYNTHETIC A") && write_doc(project, b, "SYNTHETIC B"));
    SETUP(legacy_history(project, prefix));
    const char *names[] = {a, b};
    for (size_t i = 0; i < 2; i++) {
        Revision *list = NULL;
        size_t n = 0;
        REQUIRE(!revisions_list_checked(store, project, names[i], &list, &n) && !list && !n);
        free(list);
        Revision source = {0};
        snprintf(source.file, sizeof(source.file), "20261125-000000-000-save.md");
        char *text = revisions_load(store, project, names[i], &source);
        REQUIRE(text == NULL); /* never assign legacy mixed history by basename guess or current text */
        free(text);
        bool recorded = true;
        REQUIRE(!revisions_record(store, project, names[i], "NEW SYNTHETIC", "save", NULL, &recorded) && !recorded);
    }
}

int main(void)
{
    char root[1200], a[256], b[256];
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) { fputs("TMPDIR must name an isolated approved temporary directory\n", stderr); return 2; }
    int n = snprintf(root, sizeof(root), "%s/scribodo-c02-XXXXXX", tmp);
    if (n < 0 || (size_t)n >= sizeof(root) || !mkdtemp(root)) return 2;
    StoreManager *store = store_manager_create();
    if (!store) return 2;
    memset(a, 'x', 199); memset(b, 'x', 199);
    strcpy(a + 199, "A.md"); strcpy(b + 199, "B.md");
    distinct_pair(store, root, "shared-199-byte-prefix", a, b);
    distinct_pair(store, root, "sanitized-colon", "part:one.md", "part_one.md");
    int before = failures;
    distinct_pair(store, root, "ordinary-control", "chapter-a.md", "chapter-b.md");
    if (failures != before) setup_errors++;
    legacy_cases(store, root);
    store_manager_destroy(store);
    printf("%d checks, %d failures, %d setup/control errors; isolated fixture retained at %s\n",
           checks, failures, setup_errors, root);
    return setup_errors ? 2 : failures ? 1 : 0;
}
