/* Diagnostic: the observer's text functions over stdin, one answer per line, so changes to what the judge is shown are
 * scored offline with the product's own code (tools/observer_sentence_pairs.py; GAP-001).
 *
 * usage: observer_overlap                 "A<TAB>B" lines -> obs_overlap(A, B) (-1 for a line without a tab)
 *        observer_overlap --attributes    "CANON<TAB>TEXT" lines -> 1 or 0, obs_attributes_differ(CANON, TEXT)
 *        observer_overlap --sentences     text lines -> the byte length of each sentence, space-separated
 *                                         (obs_sentence_end, whitespace between sentences skipped) */
#include <stdio.h>
#include <string.h>

#include "llm/observer.h"

int main(int argc, char **argv)
{
    const bool attributes = argc > 1 && !strcmp(argv[1], "--attributes"), sentences = argc > 1 && !strcmp(argv[1], "--sentences");
    static char line[20000];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (sentences) {
            size_t n = strlen(line), pos = 0;
            const char *sep = "";
            while (pos < n) {
                while (pos < n && (line[pos] == ' ' || line[pos] == '\t')) pos++;
                if (pos >= n) break;
                size_t e = obs_sentence_end(line + pos, n - pos);
                size_t len = e;
                while (len > 0 && (line[pos + len - 1] == ' ' || line[pos + len - 1] == '\t')) len--;
                printf("%s%zu", sep, len);
                sep = " ";
                pos += e ? e : 1;
            }
            putchar('\n');
            continue;
        }
        char *tab = strchr(line, '\t');
        if (!tab) { puts("-1"); continue; }
        *tab = '\0';
        if (attributes) puts(obs_attributes_differ(line, tab + 1) ? "1" : "0");
        else printf("%d\n", obs_overlap(line, tab + 1));
    }
    return 0;
}
