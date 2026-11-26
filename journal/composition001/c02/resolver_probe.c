/* Development-only logical-discovery experiment; never linked into Scribodo.
 * Reuse the reviewed representation and confined fixture helpers without cloning them. */
#define main representation_control_main
#include "key_probe.c"
#undef main
#include <dirent.h>

enum { MAX_HISTORY_FIXTURES = 64 };
typedef enum { EXACT_ENCODED, LEGACY_LABEL_ONLY, EXACT_LITERAL } Ownership;
typedef struct {
    char name[256], key[KEY_CAP], generation[33];
    Ownership ownership;
} LogicalHistory;
typedef struct {
    const char *const *known_names;
    size_t known_count;
    LogicalHistory items[MAX_HISTORY_FIXTURES];
    size_t count;
} Discovery;

/* Reproduces old lookup for ambiguity detection only. Never used as a new write key. */
static void legacy_key(const char *name, char key[200])
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && n < 199; p++)
        key[n++] = (*p < 0x20 || *p == ':') ? '_' : (char)*p;
    key[n] = '\0';
}

static bool legacy_ambiguous(const Discovery *d, const char *label)
{
    /* No 199-byte label proves that the old writer did not discard a suffix. */
    if (strlen(label) >= 199) return true;
    for (size_t i = 0; i < d->known_count; i++) {
        char key[200];
        legacy_key(d->known_names[i], key);
        if (!strcmp(label, key) && strcmp(label, d->known_names[i])) return true;
    }
    return false;
}

static bool append_history(Discovery *d, const char *name, const char *key, const char *generation, Ownership ownership)
{
    if (d->count == MAX_HISTORY_FIXTURES || strlen(key) >= sizeof(d->items[0].key)) return false;
    for (size_t i = 0; i < d->count; i++)
        if (!strcmp(d->items[i].name, name) && !strcmp(d->items[i].generation, generation))
            return false; /* two active authorities or two histories claiming one archive generation */
    LogicalHistory *h = &d->items[d->count++];
    snprintf(h->name, sizeof(h->name), "%s", name);
    snprintf(h->key, sizeof(h->key), "%s", key);
    snprintf(h->generation, sizeof(h->generation), "%s", generation);
    h->ownership = ownership;
    return true;
}

/* Always enumerate through a separately opened directory description, not dup(fd):
 * dup shares a directory offset, making a second scan of the same descriptor silently empty. */
static DIR *scan_dir(int fd)
{
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (copy < 0) return NULL;
    DIR *dir = fdopendir(copy);
    if (!dir) close(copy);
    return dir;
}

static bool encoded_tree(Discovery *d, int fd, const char *key, const char *generation, size_t depth)
{
    if (depth > 6) return false; /* length directory + at most five hex components */
    bool leaf = false;
    struct stat st;
    if (fstatat(fd, "index.jsonl", &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode)) return false;
        char name[256];
        if (!reversible_name(key, name, sizeof(name)) ||
            !append_history(d, name, key, generation, EXACT_ENCODED)) return false;
        leaf = true;
    } else if (errno != ENOENT) return false;
    DIR *dir = scan_dir(fd);
    if (!dir) return false;
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { ok = errno == 0; break; }
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (fstatat(fd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW)) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            if (leaf) { ok = false; break; } /* canonical length-qualified leaves have no child histories */
            char next[KEY_CAP];
            int n = snprintf(next, sizeof(next), "%s/%s", key, ent->d_name);
            if (n < 0 || (size_t)n >= sizeof(next)) { ok = false; break; }
            int child = openat(fd, ent->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            ok = child >= 0 && encoded_tree(d, child, next, generation, depth + 1);
            if (child >= 0) close(child);
        } else if (!S_ISREG(st.st_mode) || (!leaf && strcmp(ent->d_name, ".lock"))) {
            ok = false; /* symlinks/special files and misplaced metadata are not traversed */
        }
    }
    if (closedir(dir)) ok = false;
    return ok;
}

static bool valid_generation(const char *s)
{
    if (strlen(s) != 32) return false;
    for (size_t i = 0; i < 32; i++) if (unhex((unsigned char)s[i]) < 0) return false;
    return true;
}

/* Candidate C: exact filenames in a new namespace on the same filesystem as manuscripts.
 * This avoids a byte codec, but must never fall back to a lossy legacy label. */
static bool literal_tree(Discovery *d, int fd)
{
    DIR *dir = scan_dir(fd);
    if (!dir) return false;
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { ok = errno == 0; break; }
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        size_t length;
        if (!name_ok(ent->d_name, &length)) { ok = false; break; }
        int child = openat(fd, ent->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) { ok = false; break; }
        struct stat st;
        if (fstatat(child, "index.jsonl", &st, AT_SYMLINK_NOFOLLOW) == 0) {
            char key[KEY_CAP];
            int n = snprintf(key, sizeof(key), ".literal-v1/%s", ent->d_name);
            ok = S_ISREG(st.st_mode) && n > 0 && (size_t)n < sizeof(key) &&
                 append_history(d, ent->d_name, key, "", EXACT_LITERAL);
        } else ok = errno == ENOENT;
        close(child);
    }
    if (closedir(dir)) ok = false;
    return ok;
}

static bool archives(Discovery *d, int fd)
{
    DIR *dir = scan_dir(fd);
    if (!dir) return false;
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { ok = errno == 0; break; }
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (!valid_generation(ent->d_name)) { ok = false; break; }
        int generation = openat(fd, ent->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (generation < 0) { ok = false; break; }
        int names = openat(generation, ".names-v1", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(generation);
        ok = names >= 0 && encoded_tree(d, names, ".names-v1", ent->d_name, 0);
        if (names >= 0) close(names);
    }
    if (closedir(dir)) ok = false;
    return ok;
}

/* Clear all results on failure; an importer must never accept a partial ownership inventory.
 * Legacy labels remain explicitly unverified: current-file scans cannot prove deleted historical owners. */
static bool discover(int fd, const char *const *known, size_t nknown, Discovery *out)
{
    memset(out, 0, sizeof(*out));
    Discovery staged = { .known_names = known, .known_count = nknown };
    DIR *dir = scan_dir(fd);
    if (!dir) return false;
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { ok = errno == 0; break; }
        const char *label = ent->d_name;
        if (!strcmp(label, ".") || !strcmp(label, "..") || !strcmp(label, ".lock")) continue;
        int child = openat(fd, label, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) { ok = false; break; }
        if (!strcmp(label, ".names-v1")) ok = encoded_tree(&staged, child, ".names-v1", "", 0);
        else if (!strcmp(label, ".literal-v1")) ok = literal_tree(&staged, child);
        else if (!strcmp(label, ".archives-v1")) ok = archives(&staged, child);
        else {
            size_t n;
            char old[200];
            legacy_key(label, old);
            ok = name_ok(label, &n) && !strcmp(old, label) && !legacy_ambiguous(&staged, label) &&
                 append_history(&staged, label, label, "", LEGACY_LABEL_ONLY);
        }
        close(child);
    }
    if (closedir(dir)) ok = false;
    if (ok) {
        *out = staged;
        out->known_names = NULL; /* do not publish borrowed caller pointers */
        out->known_count = 0;
    }
    return ok;
}

static bool index_fixture(const char *path)
{
    int fd = open_dir(path, false);
    if (fd < 0) return false;
    int file = openat(fd, "index.jsonl", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(fd);
    if (file < 0) return false;
    /* Discovery only: not a revision parser test or an immutable source-content receipt. */
    bool ok = write(file, "{}\n", 3) == 3;
    return close(file) == 0 && ok;
}

static void literal_tests(const char *root)
{
    char fixture[1900], path[1900], key[KEY_CAP], a[256], b[256], maximum[256];
    CHECK(fixture_path(root, "literal-prototype", fixture, sizeof(fixture)) && dirs(fixture));
    memset(a, 'x', 199); strcpy(a + 199, "A.md");
    memset(b, 'x', 199); strcpy(b + 199, "B.md");
    memset(maximum, 'z', 255); maximum[255] = '\0';
    const char *names[] = {a, b, maximum, "part:one.md", "part_one.md", "retained-after-delete.md"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int n = snprintf(key, sizeof(key), ".literal-v1/%s", names[i]);
        CHECK(n > 0 && (size_t)n < sizeof(key));
        CHECK(fixture_path(fixture, key, path, sizeof(path)) && dirs(path) && index_fixture(path) && snapshot(path));
    }
    int fd = open_dir(fixture, false);
    CHECK(fd >= 0);
    if (fd < 0) return;
    Discovery result;
    CHECK(discover(fd, NULL, 0, &result) && result.count == 6);
    CHECK(discover(fd, NULL, 0, &result) && result.count == 6);
    for (size_t i = 0; i < result.count; i++) {
        CHECK(result.items[i].ownership == EXACT_LITERAL);
        CHECK(fixture_path(fixture, result.items[i].key, path, sizeof(path)) && snapshot_exists(path));
    }
    /* Two candidate formats may not silently become two active authorities for the same document. */
    CHECK(reversible_key(a, key, sizeof(key)));
    CHECK(fixture_path(fixture, key, path, sizeof(path)) && dirs(path) && index_fixture(path));
    CHECK(!discover(fd, NULL, 0, &result) && !result.count);
    CHECK(close(fd) == 0);
    printf("literal maximum relative key bytes=%zu; topology/byte checks only, no migration acceptance\n",
           sizeof(".literal-v1/") - 1 + strlen(maximum));
}

int main(void)
{
    char root[1200], path[1900], key[KEY_CAP], a[256], b[256];
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) return 2;
    int n = snprintf(root, sizeof(root), "%s/scribodo-c02-resolver-XXXXXX", tmp);
    if (n < 0 || (size_t)n >= sizeof(root) || !mkdtemp(root)) return 2;
    memset(a, 'x', 199); strcpy(a + 199, "A.md");
    memset(b, 'x', 199); strcpy(b + 199, "B.md");
    const char *names[] = {a, b, "part:one.md", "part_one.md", "deleted.md"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK(reversible_key(names[i], key, sizeof(key)));
        CHECK(fixture_path(root, key, path, sizeof(path)) && dirs(path) && index_fixture(path));
    }
    int fd = open_dir(root, false);
    CHECK(fd >= 0);
    if (fd < 0) return 2;
    Discovery result;
    CHECK(discover(fd, names, 4, &result) && result.count == 5); /* deleted document history retained */
    CHECK(discover(fd, names, 4, &result) && result.count == 5); /* independent enumeration offsets */
    for (size_t i = 0; i < result.count; i++) CHECK(result.items[i].ownership == EXACT_ENCODED);

    /* Archive generations retain the full original name; basename reuse does not merge their inventory. */
    const char *generation = "0123456789abcdef0123456789abcdef";
    char archive_key[KEY_CAP + 80];
    CHECK(reversible_key(a, key, sizeof(key)));
    n = snprintf(archive_key, sizeof(archive_key), ".archives-v1/%s/%s", generation, key);
    CHECK(n > 0 && (size_t)n < sizeof(archive_key));
    CHECK(fixture_path(root, archive_key, path, sizeof(path)) && dirs(path) && index_fixture(path));
    CHECK(discover(fd, names, 4, &result) && result.count == 6);
    size_t archived = 0;
    for (size_t i = 0; i < result.count; i++)
        if (result.items[i].generation[0]) {
            archived++;
            CHECK(!strcmp(result.items[i].generation, generation) && !strcmp(result.items[i].name, a));
        }
    CHECK(archived == 1);

    CHECK(fixture_path(root, "ordinary.md", path, sizeof(path)) && dirs(path) && index_fixture(path));
    CHECK(discover(fd, names, 4, &result) && result.count == 7);
    size_t legacy = 0;
    for (size_t i = 0; i < result.count; i++) legacy += result.items[i].ownership == LEGACY_LABEL_ONLY;
    CHECK(legacy == 1);

    /* Two active representations are an ambiguity, not permission to concatenate source lists. */
    CHECK(reversible_key("ordinary.md", key, sizeof(key)));
    CHECK(fixture_path(root, key, path, sizeof(path)) && dirs(path) && index_fixture(path));
    CHECK(!discover(fd, names, 4, &result) && !result.count);
    char index[1950];
    n = snprintf(index, sizeof(index), "%s/index.jsonl", path);
    CHECK(n > 0 && (size_t)n < sizeof(index) && unlink(index) == 0);
    CHECK(discover(fd, names, 4, &result) && result.count == 7);

    /* Known colon/underscore legacy collision refuses the entire inventory, preserving original bytes. */
    CHECK(fixture_path(root, "part_one.md", path, sizeof(path)) && dirs(path) && index_fixture(path));
    CHECK(!discover(fd, names, 4, &result) && !result.count);
    n = snprintf(index, sizeof(index), "%s/index.jsonl", path);
    CHECK(n > 0 && (size_t)n < sizeof(index) && unlink(index) == 0 && rmdir(path) == 0);
    CHECK(discover(fd, names, 4, &result) && result.count == 7);

    char prefix[200];
    memset(prefix, 'x', 199); prefix[199] = '\0';
    CHECK(fixture_path(root, prefix, path, sizeof(path)) && dirs(path) && index_fixture(path));
    CHECK(!discover(fd, NULL, 0, &result) && !result.count); /* missing current documents do not prove ownership */
    n = snprintf(index, sizeof(index), "%s/index.jsonl", path);
    CHECK(n > 0 && (size_t)n < sizeof(index) && unlink(index) == 0 && rmdir(path) == 0);

    CHECK(fixture_path(root, "linked-history", path, sizeof(path)));
    CHECK(symlink(root, path) == 0);
    CHECK(!discover(fd, names, 4, &result) && !result.count);
    CHECK(unlink(path) == 0);
    CHECK(discover(fd, names, 4, &result) && result.count == 7);

    CHECK(fixture_path(root, ".names-v1/000/61", path, sizeof(path)) && dirs(path) && index_fixture(path));
    CHECK(!discover(fd, names, 4, &result) && !result.count); /* malformed committed identity never publishes */
    n = snprintf(index, sizeof(index), "%s/index.jsonl", path);
    CHECK(n > 0 && (size_t)n < sizeof(index) && unlink(index) == 0);
    CHECK(discover(fd, names, 4, &result) && result.count == 7); /* incomplete empty containers are not histories */

    CHECK(fixture_path(root, ".archives-v1/not-a-generation", path, sizeof(path)) && dirs(path));
    CHECK(!discover(fd, names, 4, &result) && !result.count);
    CHECK(rmdir(path) == 0);
    CHECK(discover(fd, names, 4, &result) && result.count == 7);
    CHECK(close(fd) == 0);
    literal_tests(root);
    printf("%d checks, %d failures; isolated discovery fixture retained at %s\n", checks, failures, root);
    return failures ? 1 : 0;
}
