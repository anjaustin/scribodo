#define _DARWIN_C_SOURCE
#include "file_text.h"
#include "core/crash_point.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

FileTextStatus file_text_read(const char *path, char **out)
{
    *out = NULL;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) return FILE_TEXT_ERROR;
        struct stat link;
        if (lstat(path, &link) == 0 || errno != ENOENT) return FILE_TEXT_ERROR;
        return FILE_TEXT_MISSING;
    }
    struct stat before, after;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size >= SIZE_MAX) {
        close(fd);
        return FILE_TEXT_ERROR;
    }
    size_t n = (size_t)before.st_size, off = 0;
    char *text = malloc(n + 1);
    if (!text) { close(fd); return FILE_TEXT_ERROR; }
    while (off < n) {
        ssize_t got = read(fd, text + off, n - off);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        off += (size_t)got;
    }
    char extra;
    ssize_t tail;
    do tail = read(fd, &extra, 1); while (tail < 0 && errno == EINTR);
    int stable = fstat(fd, &after) == 0 && after.st_size == before.st_size &&
                 after.st_mtimespec.tv_sec == before.st_mtimespec.tv_sec &&
                 after.st_mtimespec.tv_nsec == before.st_mtimespec.tv_nsec;
    int closed = close(fd);
    if (off != n || tail != 0 || !stable || closed != 0 || memchr(text, 0, n)) {
        free(text);
        return FILE_TEXT_ERROR;
    }
    text[n] = '\0';
    *out = text;
    return FILE_TEXT_OK;
}

bool file_text_write_atomic(const char *path, const void *data, size_t n, mode_t mode)
{
    if (!path || (!data && n)) return false;
    /* Every allocation happens before the first side effect, so an allocation failure leaves no file behind
     * (the persistence fault tests inject one at each allocation and require exactly that). */
    size_t plen = strlen(path);
    char *tmp = malloc(plen + 16), *dir = malloc(plen + 2);
    if (!tmp || !dir) { free(tmp); free(dir); return false; }
    snprintf(tmp, plen + 16, "%s.tmp-XXXXXX", path);
    memcpy(dir, path, plen + 1);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else { dir[0] = '.'; dir[1] = '\0'; }
    int fd = mkstemp(tmp);
    if (fd < 0) { free(tmp); free(dir); return false; }
    bool ok = fchmod(fd, mode) == 0;
    const char *p = data;
    for (size_t off = 0; ok && off < n;) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        ok = w > 0;
        if (ok) off += (size_t)w;
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok) crash_point("atomic.temp_written"); /* the new content complete and durable beside an untouched target */
    ok = ok && rename(tmp, path) == 0;
    if (!ok) { unlink(tmp); free(tmp); free(dir); return false; }
    crash_point("atomic.renamed"); /* the target replaced; its directory entry not yet fsynced */
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    free(tmp); free(dir);
    return true;
}
