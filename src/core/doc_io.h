/* Manuscript file I/O: decoding, line-ending policy, and safe saving.
 *
 * Loading reports anything that saving could not reproduce byte-for-byte
 * (invalid UTF-8, mixed or lone-CR line endings) so the UI can warn or open
 * read-only. Saving is atomic (unique temp file in the same directory,
 * fsync, rename, fsync of the directory), writes through symlinks to the
 * real file, keeps the original permission bits, and refuses to overwrite a
 * file that changed on disk since it was loaded or last saved.
 *
 * No raylib dependency, so all of this is unit-testable.
 */
#ifndef SCRIBODO_DOC_IO_H
#define SCRIBODO_DOC_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest file we will open. A long novel is ~1-2 MB; this leaves headroom
 * while keeping every codepoint index comfortably inside an int. */
#define DOC_MAX_FILE_BYTES (64u * 1024u * 1024u)

typedef struct {
    bool exists;
    uint64_t dev, ino, size;
    int64_t mtime_sec, mtime_nsec;
} DocFileId;

typedef struct {
    uint32_t *cps;          /* malloc'd, newlines normalised to '\n' */
    size_t len;
    bool bom;
    bool crlf;              /* save with CRLF */
    bool mixed_newlines;    /* both CRLF and bare LF present */
    bool lone_cr;           /* bare CR present (converted to '\n') */
    size_t invalid_utf8;    /* sequences replaced with U+FFFD */
    bool created;           /* file did not exist; opened empty */
    bool writable;          /* we may save here (file, or its directory if new) */
    DocFileId id;
} DocLoad;

typedef enum {
    DOC_OK = 0,
    DOC_ERR_IO,
    DOC_ERR_TOO_LARGE,
    DOC_ERR_NOMEM,
    DOC_ERR_CHANGED_ON_DISK,
    DOC_ERR_READ_ONLY,      /* target is write-protected */
} DocStatus;

DocStatus doc_load(const char *path, DocLoad *out, char *err, size_t errlen);
void doc_load_free(DocLoad *d);

/* Identity of the file currently at `path` (follows symlinks). */
DocFileId doc_file_id(const char *path);
bool doc_file_id_equal(DocFileId a, DocFileId b);

/* Save `cps` to `path`. If `expected` is non-NULL and the file on disk no
 * longer matches it, nothing is written and DOC_ERR_CHANGED_ON_DISK is
 * returned; pass an id with exists == false to require that the file does
 * not exist yet. A write-protected target is refused (DOC_ERR_READ_ONLY)
 * even though rename() would let us replace it. On success `*new_id` (if
 * non-NULL) receives the saved file's id. */
DocStatus doc_save(const char *path, const uint32_t *cps, size_t n, bool bom, bool crlf,
                   const DocFileId *expected, DocFileId *new_id, char *err, size_t errlen);

/* A save that died after its temp file was complete and before the rename
 * leaves `<document>.scribodo-XXXXXX` beside the document, holding the
 * author's whole text as of that save (GAP-004 matrix, save row). Nothing is
 * deleted: on load each such file is kept and offered, renamed beside the
 * document to "<name> (Scribodo recovered <stamp>).<ext>" (the stamp is the
 * temp's modification time, a counter follows when the name is taken), never
 * overwriting a file, so it lists as a document the author can open. Only a
 * regular file named exactly <basename>.scribodo- plus six mkstemp
 * characters qualifies. Returns the number recovered; `first` (if non-NULL)
 * receives the first recovered file's name and `*differs` whether its bytes
 * differ from the document's. */
int doc_recover_strays(const char *path, char *first, size_t firstlen, bool *differs);

#endif
