/* Where to cut a text that must be split into bounded pieces.
 *
 * A piece boundary that falls inside a word or a sentence splits the fact the
 * piece carries: the half-sentence on each side matches fewer queries and
 * reads as a fragment when it is shown. text_cut moves a byte cap back to the
 * nearest natural break so each piece ends whole, without changing how much
 * of the text is covered: the next piece starts exactly where this one ends.
 *
 * Pure: no allocation, no locale, no I/O. */
#ifndef SCRIBODO_TEXT_CUT_H
#define SCRIBODO_TEXT_CUT_H

#include <stddef.h>

/* The cut to use for the piece text[0..cut) when at most `n` bytes may be
 * kept. Always a UTF-8 sequence boundary, never larger than n. When the text
 * ends within n bytes the whole length is returned. Otherwise the cut falls
 * just after the last break in the back half of the window, preferring a line
 * break, then a sentence end followed by whitespace, then any whitespace, so
 * the next piece begins on a word. With no break in the back half the plain
 * UTF-8 boundary at n is returned, so text without whitespace is cut exactly
 * as before. */
size_t text_cut(const char *text, size_t n);

#endif
