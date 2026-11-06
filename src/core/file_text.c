#define _DARWIN_C_SOURCE
#include "file_text.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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
