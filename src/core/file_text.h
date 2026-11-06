#ifndef SCRIBODO_FILE_TEXT_H
#define SCRIBODO_FILE_TEXT_H

typedef enum { FILE_TEXT_OK, FILE_TEXT_MISSING, FILE_TEXT_ERROR } FileTextStatus;

/* Complete regular-file read, caller-owned C string. Missing is distinct from
 * unreadable, special, changing or NUL-containing input; errors return NULL. */
FileTextStatus file_text_read(const char *path, char **out);

#endif
