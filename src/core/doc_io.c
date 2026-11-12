#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE /* st_mtimespec, realpath on macOS */
#define _DEFAULT_SOURCE

#include "doc_io.h"
#include "core/crash_point.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "utf8.h"

static DocFileId id_from_stat(const struct stat *st)
{
    DocFileId id = { 0 };
    id.exists = true;
    id.dev = (uint64_t)st->st_dev;
    id.ino = (uint64_t)st->st_ino;
    id.size = (uint64_t)st->st_size;
#ifdef __APPLE__
    id.mtime_sec = (int64_t)st->st_mtimespec.tv_sec;
    id.mtime_nsec = (int64_t)st->st_mtimespec.tv_nsec;
#else
    id.mtime_sec = (int64_t)st->st_mtim.tv_sec;
    id.mtime_nsec = (int64_t)st->st_mtim.tv_nsec;
#endif
    return id;
}

DocFileId doc_file_id(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return (DocFileId){ 0 };
    return id_from_stat(&st);
}

bool doc_file_id_equal(DocFileId a, DocFileId b)
{
    if (!a.exists || !b.exists) return a.exists == b.exists;
    return a.dev == b.dev && a.ino == b.ino && a.size == b.size &&
           a.mtime_sec == b.mtime_sec && a.mtime_nsec == b.mtime_nsec;
}

/* Can a new file be created next to `path`? */
static bool dir_writable(const char *path)
{
    char *dir = strdup(path);
    if (!dir) return false;
    char *slash = strrchr(dir, '/');
    const char *d = ".";
    if (slash == dir) d = "/";
    else if (slash) {
        *slash = '\0';
        d = dir;
    }
    bool ok = access(d, W_OK | X_OK) == 0;
    free(dir);
    return ok;
}

void doc_load_free(DocLoad *d)
{
    free(d->cps);
    memset(d, 0, sizeof(*d));
}

DocStatus doc_load(const char *path, DocLoad *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    err[0] = '\0';

    /* O_NONBLOCK so that opening a FIFO or device cannot hang us before
     * the regular-file check below; cleared again once we know it's a file. */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) {
            out->created = true;
            out->writable = dir_writable(path);
            out->cps = malloc(sizeof(uint32_t));
            if (!out->cps) return DOC_ERR_NOMEM;
            return DOC_OK;
        }
        snprintf(err, errlen, "Could not open %s: %s", path, strerror(errno));
        return DOC_ERR_IO;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, errlen, "%s is not a regular file", path);
        close(fd);
        return DOC_ERR_IO;
    }
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    out->writable = access(path, W_OK) == 0;
    if ((uint64_t)st.st_size > DOC_MAX_FILE_BYTES) {
        snprintf(err, errlen, "%s is too large to open (%lld MB; limit %u MB)", path,
                 (long long)(st.st_size >> 20), DOC_MAX_FILE_BYTES >> 20);
        close(fd);
        return DOC_ERR_TOO_LARGE;
    }

    size_t cap = (size_t)st.st_size + 1, len = 0;
    uint8_t *data = malloc(cap);
    if (!data) {
        close(fd);
        return DOC_ERR_NOMEM;
    }
    for (;;) {
        if (len == cap) {
            /* File grew while reading; still honour the size limit. */
            if (cap >= DOC_MAX_FILE_BYTES) {
                free(data);
                close(fd);
                snprintf(err, errlen, "%s is too large to open", path);
                return DOC_ERR_TOO_LARGE;
            }
            size_t ncap = cap * 2 > DOC_MAX_FILE_BYTES ? DOC_MAX_FILE_BYTES : cap * 2;
            uint8_t *bigger = realloc(data, ncap);
            if (!bigger) {
                free(data);
                close(fd);
                return DOC_ERR_NOMEM;
            }
            data = bigger;
            cap = ncap;
        }
        ssize_t got = read(fd, data + len, cap - len);
        if (got < 0) {
            if (errno == EINTR) continue;
            snprintf(err, errlen, "Could not read %s: %s", path, strerror(errno));
            free(data);
            close(fd);
            return DOC_ERR_IO;
        }
        if (got == 0) break;
        len += (size_t)got;
    }
    out->id = id_from_stat(&st);
    close(fd);

    size_t off = 0;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        out->bom = true;
        off = 3;
    }

    uint32_t *cps = malloc((len - off + 1) * sizeof(uint32_t));
    if (!cps) {
        free(data);
        return DOC_ERR_NOMEM;
    }
    size_t n = utf8_decode(data + off, len - off, cps, &out->invalid_utf8);
    free(data);

    size_t crlf = 0, lf = 0, w = 0;
    for (size_t r = 0; r < n; r++) {
        uint32_t c = cps[r];
        if (c == '\r') {
            if (r + 1 < n && cps[r + 1] == '\n') {
                crlf++;
                r++;
            } else {
                out->lone_cr = true;
            }
            cps[w++] = '\n';
            continue;
        }
        if (c == '\n') lf++;
        cps[w++] = c;
    }
    out->cps = cps;
    out->len = w;
    out->crlf = crlf > lf;
    out->mixed_newlines = crlf > 0 && lf > 0;
    return DOC_OK;
}

/* Write all bytes, retrying on short writes and EINTR. */
static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static uint8_t *encode(const uint32_t *cps, size_t n, bool bom, bool crlf, size_t *out_len)
{
    size_t newlines = 0;
    if (crlf)
        for (size_t i = 0; i < n; i++) newlines += cps[i] == '\n';
    size_t cap = utf8_encoded_len(cps, n) + newlines + 3;
    uint8_t *out = malloc(cap ? cap : 1);
    if (!out) return NULL;

    size_t o = 0;
    if (bom) {
        out[o++] = 0xEF; out[o++] = 0xBB; out[o++] = 0xBF;
    }
    if (!crlf) {
        o += utf8_encode(cps, n, out + o);
    } else {
        size_t start = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || cps[i] == '\n') {
                o += utf8_encode(cps + start, i - start, out + o);
                if (i < n) {
                    out[o++] = '\r';
                    out[o++] = '\n';
                }
                start = i + 1;
            }
        }
    }
    *out_len = o;
    return out;
}

/* Resolve the real file to write: follow symlinks if the target exists, so
 * saving through a link updates the linked file instead of replacing the
 * link with a regular file. */
static char *resolve_target(const char *path)
{
    char *real = realpath(path, NULL);
    if (real) return real;
    return strdup(path); /* new file */
}

static void fsync_parent_dir(const char *file)
{
    char *dir = strdup(file);
    if (!dir) return;
    char *slash = strrchr(dir, '/');
    const char *d = ".";
    if (slash == dir) d = "/";
    else if (slash) {
        *slash = '\0';
        d = dir;
    }
    int fd = open(d, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    free(dir);
}

DocStatus doc_save(const char *path, const uint32_t *cps, size_t n, bool bom, bool crlf,
                   const DocFileId *expected, DocFileId *new_id, char *err, size_t errlen)
{
    err[0] = '\0';
    char *target = resolve_target(path);
    if (!target) return DOC_ERR_NOMEM;

    struct stat st;
    bool existed = stat(target, &st) == 0;
    if (existed && !S_ISREG(st.st_mode)) {
        snprintf(err, errlen, "%s is not a regular file", path);
        free(target);
        return DOC_ERR_IO;
    }
    if (existed && access(target, W_OK) != 0) {
        snprintf(err, errlen, "%s is write-protected", path);
        free(target);
        return DOC_ERR_READ_ONLY;
    }
    if (expected) {
        DocFileId now = existed ? id_from_stat(&st) : (DocFileId){ 0 };
        if (!doc_file_id_equal(now, *expected)) {
            snprintf(err, errlen, "%s changed on disk since it was opened", path);
            free(target);
            return DOC_ERR_CHANGED_ON_DISK;
        }
    }

    size_t bytes = 0;
    uint8_t *data = encode(cps, n, bom, crlf, &bytes);
    if (!data) {
        free(target);
        return DOC_ERR_NOMEM;
    }

    size_t tlen = strlen(target) + 32;
    char *tmp = malloc(tlen);
    if (!tmp) {
        free(data);
        free(target);
        return DOC_ERR_NOMEM;
    }
    /* mkstemp creates the file with O_EXCL and mode 0600, so a pre-placed
     * symlink at the temp name cannot redirect our write. */
    snprintf(tmp, tlen, "%s.scribodo-XXXXXX", target);
    int fd = mkstemp(tmp);
    bool ok = fd >= 0;
    bool changed = false, became_read_only = false;
    int saved_errno = ok ? 0 : errno;

    if (ok && existed) ok = fchmod(fd, st.st_mode & 07777) == 0;
    if (ok) ok = write_all(fd, data, bytes);
    if (ok) ok = fsync(fd) == 0;
    if (!ok && !saved_errno) saved_errno = errno;
    if (fd >= 0 && close(fd) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    /* The new content is complete and durable in the temp file; the target is untouched (GAP-004 matrix, save row). */
    if (ok) crash_point("save.temp_written");
    /* Encoding, writing and fsync can take long enough for another program
     * to save after our first check. Revalidate at the last possible point
     * before the atomic replacement. This cannot make an uncooperative
     * cross-process check-and-rename indivisible, but it closes the material
     * write window and reduces the residual race to the rename syscall. */
    if (ok && expected && !doc_file_id_equal(doc_file_id(target), *expected)) {
        ok = false;
        changed = true;
    }
    if (ok && existed && access(target, W_OK) != 0) {
        ok = false;
        became_read_only = true;
    }
    if (ok && rename(tmp, target) != 0) {
        ok = false;
        saved_errno = errno;
    }
    /* The target now holds the new content; the directory entry is not yet fsynced. */
    if (ok) crash_point("save.renamed");

    if (!ok) {
        if (fd >= 0) unlink(tmp);
        if (changed) snprintf(err, errlen, "%s changed on disk while it was being saved", path);
        else if (became_read_only) snprintf(err, errlen, "%s became write-protected while it was being saved", path);
        else snprintf(err, errlen, "Save failed: %s", strerror(saved_errno ? saved_errno : EIO));
    } else {
        fsync_parent_dir(target);
        if (new_id) *new_id = doc_file_id(target);
    }
    free(tmp);
    free(data);
    free(target);
    return ok ? DOC_OK : changed ? DOC_ERR_CHANGED_ON_DISK : became_read_only ? DOC_ERR_READ_ONLY : DOC_ERR_IO;
}

/* ---- stray save temps --------------------------------------------------------- */

static bool mkstemp_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* `name` is `<base>.scribodo-` followed by exactly six mkstemp characters. */
static bool is_stray_of(const char *name, const char *base)
{
    size_t blen = strlen(base);
    const char *suffix = ".scribodo-";
    if (strncmp(name, base, blen) != 0 || strncmp(name + blen, suffix, strlen(suffix)) != 0) return false;
    const char *x = name + blen + strlen(suffix);
    for (int i = 0; i < 6; i++)
        if (!mkstemp_char(x[i])) return false;
    return x[6] == '\0';
}

/* Whole-file byte comparison; a missing or unreadable side counts as different. */
static bool same_bytes(const char *a, const char *b)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    bool same = fa >= 0 && fb >= 0;
    while (same) {
        unsigned char ba[4096], bb[4096];
        ssize_t na = read(fa, ba, sizeof(ba)), nb = read(fb, bb, sizeof(bb));
        same = na >= 0 && na == nb && memcmp(ba, bb, (size_t)na) == 0;
        if (na <= 0) break;
    }
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return same;
}

/* Move `from` to a name that does not exist yet: link then unlink, so an
 * existing file is never replaced (rename would). Where links are not
 * supported, an exclusive create reserves the name and rename fills it. */
static bool move_without_overwrite(const char *from, const char *to)
{
    if (link(from, to) == 0) {
        unlink(from); /* a failure here leaves a second name on the same bytes; nothing is lost */
        return true;
    }
    if (errno == EEXIST) return false;
    int fd = open(to, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    close(fd);
    if (rename(from, to) == 0) return true;
    int saved = errno;
    unlink(to);
    errno = saved;
    return false;
}

int doc_recover_strays(const char *path, char *first, size_t firstlen, bool *differs)
{
    if (first && firstlen) first[0] = '\0';
    if (differs) *differs = false;
    char *target = resolve_target(path);
    if (!target) return 0;
    char *dir = strdup(target);
    if (!dir) {
        free(target);
        return 0;
    }
    const char *base = target;
    char *slash = strrchr(dir, '/');
    const char *d = ".";
    if (slash == dir) d = "/", base = target + 1;
    else if (slash) {
        *slash = '\0';
        d = dir;
        base = target + (slash - dir) + 1;
    }
    /* "<base>" splits into stem and extension at its last dot. */
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) dot = base + strlen(base);

    int recovered = 0;
    DIR *dp = opendir(d);
    if (!dp) {
        free(dir);
        free(target);
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (!is_stray_of(ent->d_name, base)) continue;
        char from[PATH_MAX + 300];
        snprintf(from, sizeof(from), "%s/%s", d, ent->d_name);
        struct stat st;
        if (lstat(from, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        struct tm tmv;
#ifdef __APPLE__
        time_t when = st.st_mtimespec.tv_sec;
#else
        time_t when = st.st_mtim.tv_sec;
#endif
        localtime_r(&when, &tmv);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M%S", &tmv);
        char name[PATH_MAX + 300], to[PATH_MAX + 600];
        bool moved = false;
        for (int k = 1; k < 1000 && !moved; k++) {
            if (k == 1) snprintf(name, sizeof(name), "%.*s (Scribodo recovered %s)%s", (int)(dot - base), base, stamp, dot);
            else snprintf(name, sizeof(name), "%.*s (Scribodo recovered %s, %d)%s", (int)(dot - base), base, stamp, k, dot);
            snprintf(to, sizeof(to), "%s/%s", d, name);
            moved = move_without_overwrite(from, to);
            if (!moved && errno != EEXIST) break;
        }
        if (!moved) continue;
        if (recovered == 0) {
            if (first && firstlen) snprintf(first, firstlen, "%s", name);
            if (differs) *differs = !same_bytes(to, target);
        }
        recovered++;
    }
    closedir(dp);
    if (recovered) fsync_parent_dir(target);
    free(dir);
    free(target);
    return recovered;
}
