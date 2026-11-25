/* Isolated native representation experiment. Not linked into the product.
 * Tests names as opaque bytes; does not change manuscript or history data. */
#define _DARWIN_C_SOURCE
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { NAME_MAX_BYTES = 255, KEY_CAP = 600, HEX_COMPONENT = 120 };
static int checks, failures;
#define CHECK(test) do { checks++; if (!(test)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); } } while (0)
static const char hex[] = "0123456789abcdef";

static bool name_ok(const char *name, size_t *n)
{
    if (!name) return false;
    *n = strnlen(name, NAME_MAX_BYTES + 1);
    return *n && *n <= NAME_MAX_BYTES && name[0] != '.' && !strchr(name, '/');
}

/* Candidate A: self-describing, injective, filesystem-independent byte encoding.
 * Length separates prefix names, so a document leaf can never contain another document's leaf. */
static bool reversible_key(const char *name, char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    size_t n;
    if (!name_ok(name, &n)) return false;
    char key[KEY_CAP];
    int head = snprintf(key, sizeof(key), ".names-v1/%03zu/", n);
    if (head < 0 || (size_t)head >= sizeof(key)) return false;
    size_t k = (size_t)head;
    for (size_t i = 0; i < n; i++) {
        if (i && (2 * i) % HEX_COMPONENT == 0) key[k++] = '/';
        unsigned char c = (unsigned char)name[i];
        key[k++] = hex[c >> 4];
        key[k++] = hex[c & 15];
    }
    key[k] = '\0';
    if (k + 1 > cap) return false;
    memcpy(out, key, k + 1);
    return true;
}

static int unhex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1; /* reject noncanonical uppercase too */
}

static bool reversible_name(const char *key, char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    const char prefix[] = ".names-v1/";
    if (!key || strncmp(key, prefix, sizeof(prefix) - 1)) return false;
    size_t len = strnlen(key, KEY_CAP);
    if (len < sizeof(prefix) - 1 + 4 || len == KEY_CAP) return false;
    size_t k = sizeof(prefix) - 1, n = 0;
    for (size_t i = 0; i < 3; i++) {
        unsigned char c = (unsigned char)key[k++];
        if (c < '0' || c > '9') return false;
        n = n * 10 + c - '0';
    }
    if (key[k++] != '/' || !n || n > NAME_MAX_BYTES || cap <= n) return false;
    char name[NAME_MAX_BYTES + 1];
    for (size_t i = 0; i < n; i++) {
        if (i && (2 * i) % HEX_COMPONENT == 0) {
            if (k >= len || key[k++] != '/') return false;
        }
        if (k + 2 > len) return false;
        int hi = unhex((unsigned char)key[k++]), lo = unhex((unsigned char)key[k++]);
        if (hi < 0 || lo < 0) return false;
        name[i] = (char)((hi << 4) | lo);
        if (!name[i]) return false;
    }
    name[n] = '\0';
    size_t actual;
    if (k != len || !name_ok(name, &actual) || actual != n) return false;
    memcpy(out, name, n + 1);
    return true;
}

/* Candidate B: shallow digest lookup plus separately validated full ownership.
 * The key alone is never accepted as ownership evidence. */
static bool digest_key(const char *name, char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    size_t n;
    if (!name_ok(name, &n)) return false;
    const char prefix[] = ".names-h1/";
    size_t k = sizeof(prefix) - 1;
    if (cap < k + CC_SHA256_DIGEST_LENGTH * 2 + 1) return false;
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    if (!CC_SHA256(name, (CC_LONG)n, hash)) return false;
    memcpy(out, prefix, k);
    for (size_t i = 0; i < sizeof(hash); i++) {
        out[k++] = hex[hash[i] >> 4];
        out[k++] = hex[hash[i] & 15];
    }
    out[k] = '\0';
    return true;
}

static bool owner_matches(const char *key, const char *name, const char *owner, size_t owner_bytes)
{
    char expected[KEY_CAP];
    size_t n;
    return name_ok(name, &n) && owner && n == owner_bytes && !memcmp(name, owner, n) &&
           digest_key(name, expected, sizeof(expected)) && key && !strcmp(key, expected);
}

static bool dirs(const char *path)
{
    char copy[1900];
    if (strlen(path) >= sizeof(copy)) return false;
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; ; p++) {
        if (*p && *p != '/') continue;
        char saved = *p;
        *p = '\0';
        if (mkdir(copy, 0700) && errno != EEXIST) return false;
        *p = saved;
        if (!saved) return true;
    }
}

static bool fixture_path(const char *root, const char *key, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", root, key);
    return n > 0 && (size_t)n < cap;
}

static bool snapshot(const char *dir)
{
    char path[2000];
    int n = snprintf(path, sizeof(path), "%s/snapshot.md", dir);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    FILE *f = fopen(path, "wx");
    if (!f) return false;
    bool ok = fputs("SYNTHETIC SNAPSHOT", f) >= 0;
    return fclose(f) == 0 && ok;
}

static bool snapshot_exists(const char *dir)
{
    char path[2000];
    int n = snprintf(path, sizeof(path), "%s/snapshot.md", dir);
    return n > 0 && (size_t)n < sizeof(path) && access(path, F_OK) == 0;
}

static void codec_tests(void)
{
    char name[NAME_MAX_BYTES + 2], other[NAME_MAX_BYTES + 2], a[KEY_CAP], b[KEY_CAP], decoded[256];
    for (size_t n = 1; n <= NAME_MAX_BYTES; n++) {
        memset(name, 'x', n);
        name[n] = '\0';
        memcpy(other, name, n + 1);
        other[n - 1] = 'y';
        CHECK(reversible_key(name, a, sizeof(a)));
        CHECK(reversible_name(a, decoded, sizeof(decoded)) && !strcmp(name, decoded));
        CHECK(reversible_key(other, b, sizeof(b)) && strcmp(a, b));
        /* Every undersized output must refuse with an empty string, not expose a truncated key. */
        size_t required = strlen(a);
        for (size_t cap = 1; cap <= required; cap++) {
            b[0] = '?';
            CHECK(!reversible_key(name, b, cap) && !b[0]);
        }
        CHECK(!reversible_name(a, decoded, n) && !decoded[0]);
        CHECK(digest_key(name, b, sizeof(b)));
        CHECK(owner_matches(b, name, name, n));
        CHECK(!owner_matches(b, name, other, n));
        CHECK(!owner_matches(b, name, name, n - 1));
        CHECK(!owner_matches(a, name, name, n));
    }
    const char *names[] = { "part:one.md", "part_one.md", "A.md", "a.md",
                            "e\xcc\x81.md", "\xc3\xa9.md", "emoji-\xf0\x9f\x8c\x99.md" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK(reversible_key(names[i], a, sizeof(a)));
        CHECK(reversible_name(a, decoded, sizeof(decoded)) && !strcmp(names[i], decoded));
        for (size_t j = 0; j < i; j++) {
            CHECK(reversible_key(names[j], b, sizeof(b)) && strcmp(a, b));
        }
    }
    const char *invalid[] = { "", ".", "..", ".names-v1", "a/b.md" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        CHECK(!reversible_key(invalid[i], a, sizeof(a)) && !a[0]);
        CHECK(!digest_key(invalid[i], a, sizeof(a)) && !a[0]);
    }
    memset(name, 'z', 256); name[256] = '\0';
    CHECK(!reversible_key(name, a, sizeof(a)));
    CHECK(!digest_key(name, a, sizeof(a)));
    const char *bad_keys[] = {
        ".names-v1/000/", ".names-v1/256/61", ".names-v1/001/00", ".names-v1/001/2e",
        ".names-v1/001/2f", ".names-v1/001/6A", ".names-v1/001/6", ".names-v1/001/61/",
        ".names-v1/002/61", ".names-v1/001/6162", ".names-v1/../61", ".names-v1/01/61"
    };
    for (size_t i = 0; i < sizeof(bad_keys) / sizeof(bad_keys[0]); i++)
        CHECK(!reversible_name(bad_keys[i], decoded, sizeof(decoded)) && !decoded[0]);
}

static void filesystem_tests(const char *root)
{
    char names[3][256], keys[3][KEY_CAP], paths[3][1900];
    memset(names[0], 'x', 199); strcpy(names[0] + 199, "A.md");
    memset(names[1], 'x', 199); strcpy(names[1] + 199, "B.md");
    memset(names[2], 'x', 255); names[2][255] = '\0';
    for (size_t i = 0; i < 3; i++) {
        CHECK(reversible_key(names[i], keys[i], sizeof(keys[i])));
        CHECK(fixture_path(root, keys[i], paths[i], sizeof(paths[i])));
        CHECK(dirs(paths[i]));
        CHECK(snapshot(paths[i]));
    }
    /* A length-qualified encoding prevents one leaf from owning a prefix-name sibling. */
    char shorter[256], key[KEY_CAP], path[1900];
    memset(shorter, 'x', 198); shorter[198] = '\0';
    CHECK(reversible_key(shorter, key, sizeof(key)));
    CHECK(fixture_path(root, key, path, sizeof(path)) && dirs(path) && snapshot(path));
    CHECK(strncmp(paths[0], path, strlen(path)) != 0);

    /* Rename/archive here exercise directory topology only, not author/source transaction recovery. */
    char moved[1900], target[1900], parent[1900];
    CHECK(reversible_key("renamed.md", key, sizeof(key)));
    CHECK(fixture_path(root, key, moved, sizeof(moved)));
    snprintf(parent, sizeof(parent), "%s", moved);
    char *slash = strrchr(parent, '/');
    CHECK(slash != NULL);
    if (!slash) return;
    *slash = '\0';
    CHECK(dirs(parent));
    CHECK(rename(paths[0], moved) == 0);
    CHECK(snapshot_exists(moved) && snapshot_exists(paths[1]) && snapshot_exists(paths[2]) && snapshot_exists(path));
    int n = snprintf(target, sizeof(target), "%s/.archives-v1/generation-one", root);
    CHECK(n > 0 && (size_t)n < sizeof(target) && dirs(target));
    n = snprintf(target, sizeof(target), "%s/.archives-v1/generation-one/history", root);
    CHECK(n > 0 && (size_t)n < sizeof(target) && rename(moved, target) == 0);
    CHECK(snapshot_exists(target) && snapshot_exists(paths[1]));
    printf("reversible maximum key bytes=%zu; digest key bytes=%zu (path arithmetic, not measured RAM/latency)\n",
           strlen(keys[2]), sizeof(".names-h1/") - 1 + CC_SHA256_DIGEST_LENGTH * 2);
}

int main(void)
{
    char root[1200];
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) return 2;
    int n = snprintf(root, sizeof(root), "%s/scribodo-c02-keys-XXXXXX", tmp);
    if (n < 0 || (size_t)n >= sizeof(root) || !mkdtemp(root)) return 2;
    codec_tests();
    filesystem_tests(root);
    printf("%d checks, %d failures; synthetic fixture retained at %s\n", checks, failures, root);
    return failures ? 1 : 0;
}
