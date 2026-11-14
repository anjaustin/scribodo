/* File load/save tests against the real filesystem (in a temp directory). */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/core/doc_io.h"
#include "../src/core/file_text.h"
#include "test_util.h"

static char g_dir[512];
static char g_err[512];

static const char *tmp_path(const char *name)
{
    static char buf[4][1024];
    static int k = 0;
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", g_dir, name);
    return buf[k];
}

static void write_bytes(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}

static size_t read_bytes(const char *path, uint8_t *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return (size_t)-1;
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

static int count_temp_files(void)
{
    DIR *d = opendir(g_dir);
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        if (strstr(ent->d_name, ".scribodo-")) n++;
    closedir(d);
    return n;
}

/* Load, then save unchanged; bytes must be identical. */
static void roundtrip_bytes(const char *name, const char *bytes, size_t n)
{
    const char *p = tmp_path(name);
    write_bytes(p, bytes, n);
    DocLoad d;
    CHECK(doc_load(p, &d, g_err, sizeof(g_err)) == DOC_OK);
    CHECK(doc_save(p, d.cps, d.len, d.bom, d.crlf, &d.id, NULL, g_err, sizeof(g_err)) == DOC_OK);
    uint8_t back[4096];
    size_t m = read_bytes(p, back, sizeof(back));
    CHECK(m == n && memcmp(back, bytes, n) == 0);
    doc_load_free(&d);
}

static void test_roundtrip_preserves_bytes(void)
{
    const char lf[] = "Title\n\nZoë said “hi” — then left.\n";
    roundtrip_bytes("lf.md", lf, sizeof(lf) - 1);

    const char crlf_bom[] = "\xEF\xBB\xBF" "Line one\r\nLine two\r\n\r\nÅngström\r\n";
    roundtrip_bytes("crlf_bom.md", crlf_bom, sizeof(crlf_bom) - 1);

    const char no_trailing[] = "no trailing newline";
    roundtrip_bytes("nonl.txt", no_trailing, sizeof(no_trailing) - 1);

    roundtrip_bytes("empty.txt", "", 0);
    CHECK(count_temp_files() == 0);
}

static void test_load_flags(void)
{
    DocLoad d;
    const char *p = tmp_path("mixed.md");
    write_bytes(p, "a\r\nb\nc\r\n", 8);
    CHECK(doc_load(p, &d, g_err, sizeof(g_err)) == DOC_OK);
    CHECK(d.mixed_newlines && d.crlf && !d.lone_cr && d.len == 6);
    doc_load_free(&d);

    p = tmp_path("cr.md");
    write_bytes(p, "a\rb", 3);
    CHECK(doc_load(p, &d, g_err, sizeof(g_err)) == DOC_OK);
    CHECK(d.lone_cr && d.len == 3 && d.cps[1] == '\n');
    doc_load_free(&d);

    p = tmp_path("bad.md");
    write_bytes(p, "ok \xFF\xFE bad", 9);
    CHECK(doc_load(p, &d, g_err, sizeof(g_err)) == DOC_OK);
    CHECK(d.invalid_utf8 == 2);
    doc_load_free(&d);

    CHECK(doc_load(tmp_path("missing.md"), &d, g_err, sizeof(g_err)) == DOC_OK);
    CHECK(d.created && d.len == 0 && !d.id.exists);
    doc_load_free(&d);
}

static void test_rejects_non_regular_and_large(void)
{
    DocLoad d;
    const char *sub = tmp_path("subdir");
    mkdir(sub, 0755);
    CHECK(doc_load(sub, &d, g_err, sizeof(g_err)) == DOC_ERR_IO);

    /* Sparse file just over the limit: rejected without reading it. */
    const char *big = tmp_path("big.txt");
    int fd = open(big, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && ftruncate(fd, (off_t)DOC_MAX_FILE_BYTES + 1) == 0);
    close(fd);
    CHECK(doc_load(big, &d, g_err, sizeof(g_err)) == DOC_ERR_TOO_LARGE);
    unlink(big);

    uint32_t cps[] = { 'x' };
    CHECK(doc_save(sub, cps, 1, false, false, NULL, NULL, g_err, sizeof(g_err)) == DOC_ERR_IO);
}

static void test_save_through_symlink(void)
{
    const char *real = tmp_path("real.md");
    const char *link = tmp_path("link.md");
    write_bytes(real, "old\n", 4);
    unlink(link);
    CHECK(symlink(real, link) == 0);

    DocLoad d;
    CHECK(doc_load(link, &d, g_err, sizeof(g_err)) == DOC_OK);
    uint32_t cps[] = { 'n', 'e', 'w', '\n' };
    CHECK(doc_save(link, cps, 4, false, false, &d.id, NULL, g_err, sizeof(g_err)) == DOC_OK);
    doc_load_free(&d);

    struct stat st;
    CHECK(lstat(link, &st) == 0 && S_ISLNK(st.st_mode)); /* still a link */
    uint8_t buf[16];
    CHECK(read_bytes(real, buf, sizeof(buf)) == 4 && memcmp(buf, "new\n", 4) == 0);
}

static void test_save_preserves_mode(void)
{
    const char *p = tmp_path("mode.md");
    write_bytes(p, "x", 1);
    chmod(p, 0640);
    uint32_t cps[] = { 'y' };
    CHECK(doc_save(p, cps, 1, false, false, NULL, NULL, g_err, sizeof(g_err)) == DOC_OK);
    struct stat st;
    CHECK(stat(p, &st) == 0 && (st.st_mode & 07777) == 0640);
}

static void test_refuses_external_change(void)
{
    const char *p = tmp_path("ext.md");
    write_bytes(p, "mine\n", 5);
    DocLoad d;
    CHECK(doc_load(p, &d, g_err, sizeof(g_err)) == DOC_OK);

    /* Another program rewrites the file. */
    write_bytes(p, "theirs, longer\n", 15);

    uint32_t cps[] = { 'm', 'e', '\n' };
    DocFileId id;
    CHECK(doc_save(p, cps, 3, false, false, &d.id, &id, g_err, sizeof(g_err)) == DOC_ERR_CHANGED_ON_DISK);
    uint8_t buf[32];
    CHECK(read_bytes(p, buf, sizeof(buf)) == 15); /* untouched */

    /* Explicit overwrite (no expectation) succeeds and reports the new id. */
    CHECK(doc_save(p, cps, 3, false, false, NULL, &id, g_err, sizeof(g_err)) == DOC_OK);
    CHECK(doc_file_id_equal(id, doc_file_id(p)));

    /* A file created at our path after we opened it as new is also caught. */
    DocLoad fresh;
    const char *np = tmp_path("appeared.md");
    CHECK(doc_load(np, &fresh, g_err, sizeof(g_err)) == DOC_OK && fresh.created);
    write_bytes(np, "surprise", 8);
    CHECK(doc_save(np, cps, 3, false, false, &fresh.id, NULL, g_err, sizeof(g_err)) == DOC_ERR_CHANGED_ON_DISK);
    doc_load_free(&fresh);
    doc_load_free(&d);
}

static void test_failed_save_leaves_original(void)
{
    if (geteuid() == 0) return; /* root ignores directory permissions */
    const char *sub = tmp_path("locked");
    mkdir(sub, 0755);
    char file[1024];
    snprintf(file, sizeof(file), "%s/doc.md", sub);
    write_bytes(file, "original", 8);
    chmod(sub, 0555); /* no new files (temp) allowed */

    uint32_t cps[] = { 'z' };
    CHECK(doc_save(file, cps, 1, false, false, NULL, NULL, g_err, sizeof(g_err)) == DOC_ERR_IO);
    uint8_t buf[16];
    CHECK(read_bytes(file, buf, sizeof(buf)) == 8 && memcmp(buf, "original", 8) == 0);
    chmod(sub, 0755);
}

/* Review finding R4: a FIFO must be refused, not block forever. main()
 * arms an alarm so a regression fails the run instead of hanging it. */
/* The one atomic writer for the app's small files (core/file_text.h): whole content, requested mode, no temp
 * left, the original kept when the write cannot happen. */
/* The draft writer leaves exactly one save-temp-named file beside the document, which recovery then offers. */
static void test_write_draft(void)
{
    char dir[600], doc[700], first[600], d1[800], d2[800];
    snprintf(dir, sizeof(dir), "%s/drafts", g_dir);
    CHECK(mkdir(dir, 0755) == 0);
    snprintf(doc, sizeof(doc), "%s/chapter.md", dir);
    write_bytes(doc, "old\n", 4);
    CHECK(doc_write_draft(doc, "first draft\n", 12, "", d1, sizeof(d1)));
    const char *tail = strrchr(d1, '/'); /* the path is resolved (realpath), so compare the name */
    CHECK(tail && !strncmp(tail, "/chapter.md.scribodo-", 21) && strlen(tail) == 27);
    uint8_t buf[64];
    CHECK(read_bytes(d1, buf, sizeof(buf)) == 12 && !memcmp(buf, "first draft\n", 12));
    CHECK(doc_write_draft(doc, "second draft\n", 13, d1, d2, sizeof(d2)));
    CHECK(strcmp(d1, d2) != 0 && access(d1, F_OK) != 0 && read_bytes(d2, buf, sizeof(buf)) == 13);
    CHECK(read_bytes(doc, buf, sizeof(buf)) == 4); /* the document itself is untouched */
    bool differs = false;
    CHECK(doc_recover_strays(doc, first, sizeof(first), &differs) == 1 && differs);
    CHECK(!strncmp(first, "chapter (Scribodo recovered ", 28));
    char tiny[8];
    CHECK(!doc_write_draft(doc, "x", 1, "", tiny, sizeof(tiny)) && tiny[0] == '\0');
    CHECK(!doc_write_draft(NULL, "x", 1, "", d1, sizeof(d1)));
}

/* A stray save temp beside a document is kept under a name that lists as a document, never over a file. */
static void test_recover_strays(void)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/strays", g_dir);
    CHECK(mkdir(dir, 0755) == 0);
    char doc[700], stray[800], other[800], first[600];
    snprintf(doc, sizeof(doc), "%s/chapter.md", dir);
    write_bytes(doc, "old\n", 4);
    snprintf(stray, sizeof(stray), "%s/chapter.md.scribodo-Ab3dEf", dir);
    write_bytes(stray, "new text\n", 9);
    /* Names that are not a stray of this document stay where they are. */
    snprintf(other, sizeof(other), "%s/chapter.md.scribodo-short", dir); write_bytes(other, "x", 1);
    snprintf(other, sizeof(other), "%s/chapter.md.scribodo-toolong", dir); write_bytes(other, "x", 1);
    snprintf(other, sizeof(other), "%s/chapter.md.scribodo-Ab3d-f", dir); write_bytes(other, "x", 1);
    snprintf(other, sizeof(other), "%s/other.md.scribodo-Ab3dEf", dir); write_bytes(other, "x", 1);
    snprintf(other, sizeof(other), "%s/chapter.md.scribodo-DirXYZ", dir); CHECK(mkdir(other, 0755) == 0);
    bool differs = false;
    CHECK(doc_recover_strays(doc, first, sizeof(first), &differs) == 1);
    CHECK(differs);
    CHECK(!strncmp(first, "chapter (Scribodo recovered ", 28) && !strcmp(first + strlen(first) - 4, ").md"));
    char moved[1400];
    snprintf(moved, sizeof(moved), "%s/%s", dir, first);
    uint8_t buf[64];
    CHECK(read_bytes(moved, buf, sizeof(buf)) == 9 && !memcmp(buf, "new text\n", 9));
    CHECK(read_bytes(doc, buf, sizeof(buf)) == 4 && !memcmp(buf, "old\n", 4)); /* the document is untouched */
    CHECK(access(stray, F_OK) != 0);
    int left = 0;
    DIR *d = opendir(dir);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) if (strstr(ent->d_name, ".scribodo-")) left++;
    closedir(d);
    CHECK(left == 5); /* the four look-alikes and the directory */
    /* Nothing more to recover; the first-name field is cleared. */
    CHECK(doc_recover_strays(doc, first, sizeof(first), &differs) == 0 && first[0] == '\0' && !differs);
    /* A second stray with the same stamp takes a counter; a stray with the document's bytes is reported as such. */
    write_bytes(stray, "old\n", 4);
    struct stat st;
    CHECK(stat(moved, &st) == 0);
    struct timespec times[2] = { st.st_mtimespec, st.st_mtimespec };
    CHECK(utimensat(AT_FDCWD, stray, times, 0) == 0);
    CHECK(doc_recover_strays(doc, first, sizeof(first), &differs) == 1);
    CHECK(!differs && strstr(first, ", 2).md") != NULL);
    CHECK(read_bytes(moved, buf, sizeof(buf)) == 9); /* the earlier recovered file was not replaced */
    /* A document without an extension, and one opened through a symlink (the stray lies beside the real file). */
    char plain[700], plain_stray[800], link_path[700];
    snprintf(plain, sizeof(plain), "%s/notes", dir); write_bytes(plain, "n", 1);
    snprintf(plain_stray, sizeof(plain_stray), "%s/notes.scribodo-000000", dir); write_bytes(plain_stray, "m", 1);
    snprintf(link_path, sizeof(link_path), "%s/notes-link", dir);
    CHECK(symlink(plain, link_path) == 0);
    CHECK(doc_recover_strays(link_path, first, sizeof(first), &differs) == 1 && differs);
    CHECK(!strncmp(first, "notes (Scribodo recovered ", 26) && first[strlen(first) - 1] == ')');
    /* No arguments wanted, no document: nothing happens. */
    snprintf(other, sizeof(other), "%s/missing.md", dir);
    CHECK(doc_recover_strays(other, NULL, 0, NULL) == 0);
}

static void test_file_text_write_atomic(void)
{
    const char *p = tmp_path("atomic.json");
    CHECK(file_text_write_atomic(p, "first", 5, 0644));
    uint8_t buf[64];
    CHECK(read_bytes(p, buf, sizeof(buf)) == 5 && memcmp(buf, "first", 5) == 0);
    struct stat st;
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0644);
    CHECK(file_text_write_atomic(p, "second, longer", 14, 0600));
    CHECK(read_bytes(p, buf, sizeof(buf)) == 14 && memcmp(buf, "second, longer", 14) == 0);
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(file_text_write_atomic(p, NULL, 0, 0644) && read_bytes(p, buf, sizeof(buf)) == 0); /* an empty file is a write */
    CHECK(count_temp_files() == 0);
    CHECK(!file_text_write_atomic(NULL, "x", 1, 0644) && !file_text_write_atomic(p, NULL, 1, 0644));
    if (geteuid() != 0) {
        const char *sub = tmp_path("atomic-locked");
        mkdir(sub, 0755);
        char file[1024];
        snprintf(file, sizeof(file), "%s/settings.json", sub);
        CHECK(file_text_write_atomic(file, "kept", 4, 0644));
        chmod(sub, 0555);
        CHECK(!file_text_write_atomic(file, "lost", 4, 0644));
        CHECK(read_bytes(file, buf, sizeof(buf)) == 4 && memcmp(buf, "kept", 4) == 0);
        chmod(sub, 0755);
    }
}

static void test_fifo_does_not_block(void)
{
    const char *p = tmp_path("pipe.md");
    unlink(p);
    CHECK(mkfifo(p, 0644) == 0);
    DocLoad d;
    CHECK(doc_load(p, &d, g_err, sizeof(g_err)) == DOC_ERR_IO);
    unlink(p);
}

/* Review finding R2: rename() could replace a write-protected file; refuse. */
static void test_write_protected(void)
{
    const char *p = tmp_path("ro.md");
    write_bytes(p, "keep", 4);
    chmod(p, 0444);
    DocLoad d;
    CHECK(doc_load(p, &d, g_err, sizeof(g_err)) == DOC_OK);
    CHECK(!d.writable || geteuid() == 0);
    uint32_t cps[] = { 'x' };
    if (geteuid() != 0)
        CHECK(doc_save(p, cps, 1, false, false, NULL, NULL, g_err, sizeof(g_err)) == DOC_ERR_READ_ONLY);
    uint8_t buf[8];
    CHECK(read_bytes(p, buf, sizeof(buf)) == 4 && memcmp(buf, "keep", 4) == 0);
    chmod(p, 0644);
    doc_load_free(&d);
}

static void test_large_roundtrip(void)
{
    /* ~3 MB mixed-script manuscript, CRLF. */
    const char *para = "Il était une fois — “quoted” — Привет, 日本. ";
    size_t plen = strlen(para);
    size_t reps = 60000;
    char *src = malloc(reps * (plen + 2));
    size_t n = 0;
    for (size_t i = 0; i < reps; i++) {
        memcpy(src + n, para, plen);
        n += plen;
        if (i % 7 == 6) {
            src[n++] = '\r';
            src[n++] = '\n';
        }
    }
    const char *p = tmp_path("large.md");
    write_bytes(p, src, n);
    DocLoad d;
    CHECK(doc_load(p, &d, g_err, sizeof(g_err)) == DOC_OK && d.crlf);
    CHECK(doc_save(p, d.cps, d.len, d.bom, d.crlf, &d.id, NULL, g_err, sizeof(g_err)) == DOC_OK);
    uint8_t *back = malloc(n + 16);
    CHECK(read_bytes(p, back, n + 16) == n && memcmp(back, src, n) == 0);
    free(back);
    free(src);
    doc_load_free(&d);
}

static void rm_rf(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(path);
        else unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/scribodo-test-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }
    alarm(20); /* any blocking I/O regression fails instead of hanging */
    TestCase tests[] = {
        { "roundtrip_preserves_bytes", test_roundtrip_preserves_bytes },
        { "load_flags", test_load_flags },
        { "rejects_non_regular_and_large", test_rejects_non_regular_and_large },
        { "save_through_symlink", test_save_through_symlink },
        { "save_preserves_mode", test_save_preserves_mode },
        { "refuses_external_change", test_refuses_external_change },
        { "failed_save_leaves_original", test_failed_save_leaves_original },
        { "recover_strays", test_recover_strays }, { "write_draft", test_write_draft },
        { "file_text_write_atomic", test_file_text_write_atomic }, { "fifo_does_not_block", test_fifo_does_not_block },
        { "write_protected", test_write_protected },
        { "large_roundtrip", test_large_roundtrip },
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    rm_rf(g_dir);
    return rc;
}
