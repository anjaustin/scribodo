/* Reading font files without a font engine: how many faces a file holds, each face's family and style names, and a
 * standalone copy of one face of a collection.
 *
 * raylib (stb_truetype underneath) loads only a file's first face and only by a ".ttf"/".otf" name, so a collection
 * (.ttc: Helvetica, Avenir, Menlo, Charter and most of macOS's own fonts) cannot be loaded as it stands.
 * font_file_extract_face writes the face's tables into a standalone sfnt that it can load from memory.
 *
 * Every function takes the whole file as bytes and treats them as untrusted: each offset and length is checked
 * against the buffer before it is read, and a malformed file is refused, never read past its end
 * (tests/test_font_file.c truncates and corrupts real files to check). */
#ifndef SCRIBODO_FONT_FILE_H
#define SCRIBODO_FONT_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FONT_FILE_MAX_FACES 256

typedef struct {
    char family[128]; /* UTF-8; the typographic family (name ID 16) when present, else the family (ID 1) */
    char style[64];   /* the typographic subfamily (ID 17), else the subfamily (ID 2): "Regular", "Bold Italic" */
} FontFaceName;

/* Faces in the file: 1 for a .ttf/.otf (sfnt version 0x00010000, 'OTTO' or 'true'), the count for a collection
 * ('ttcf', at most FONT_FILE_MAX_FACES), 0 when the bytes are not a font this reads. */
int font_file_face_count(const uint8_t *data, size_t len);

/* Face `index`'s names. False when the face or its name table is missing or malformed, or no family name is found. */
bool font_file_face_name(const uint8_t *data, size_t len, int index, FontFaceName *out);

/* True when face `index` maps the codepoint (its cmap: format 4 or 12, Unicode or Windows platform). */
bool font_file_has_codepoint(const uint8_t *data, size_t len, int index, uint32_t cp);

/* True when face `index` draws with outlines ('glyf' or 'CFF ') and is not a colour-bitmap face ('sbix', 'CBDT'):
 * Apple Color Emoji keeps a 'glyf' table of empty placeholders beside its 'sbix' bitmaps, and stb_truetype draws
 * nothing from it. */
bool font_file_has_outlines(const uint8_t *data, size_t len, int index);

/* Face `index` as a standalone sfnt in a new malloc'd buffer (*out_len bytes), its tables copied 4-byte aligned and
 * its table directory rewritten for the new offsets; the caller frees it. For a single-face file this is a copy.
 * NULL when the face is malformed, a table lies outside the file, or the tables together exceed the file's size (they
 * overlap). */
uint8_t *font_file_extract_face(const uint8_t *data, size_t len, int index, size_t *out_len);

#endif
