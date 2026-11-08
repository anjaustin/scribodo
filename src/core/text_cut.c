#include "text_cut.h"

#include <stdbool.h>

static bool continuation(unsigned char c) { return (c & 0xc0) == 0x80; }
static bool space(unsigned char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
static bool sentence_end(unsigned char c) { return c == '.' || c == '!' || c == '?'; }

size_t text_cut(const char *text, size_t n)
{
    if (!text) return 0;
    size_t len = 0;
    while (len < n && text[len]) len++;
    if (len < n || !text[n]) return len; /* the whole text fits: no cut */
    while (n && continuation((unsigned char)text[n])) --n;
    size_t floor = n / 2, line = 0, sentence = 0, word = 0;
    for (size_t i = n; i > floor && !line; i--) {
        unsigned char c = (unsigned char)text[i - 1];
        if (c == '\n') {
            line = i;
        } else if (space(c)) {
            if (!word) word = i;
            if (!sentence && i >= 2 && sentence_end((unsigned char)text[i - 2])) sentence = i;
        }
    }
    if (line) return line;
    if (sentence) return sentence;
    if (word) return word;
    return n;
}
