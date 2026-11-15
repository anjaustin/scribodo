/* Diagnostic: the observer's canon candidate score, obs_overlap (src/llm/observer_text.c), over "A<TAB>B" lines on
 * stdin, one score per line on stdout (-1 for a line without a tab), so a change to what the judge is shown is scored
 * offline with the product's own function (tools/observer_sentence_pairs.py; GAP-001). */
#include <stdio.h>
#include <string.h>

#include "llm/observer.h"

int main(void)
{
    static char line[20000];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) { puts("-1"); continue; }
        *tab = '\0';
        printf("%d\n", obs_overlap(line, tab + 1));
    }
    return 0;
}
