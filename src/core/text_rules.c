/* Shared text rules (AUDIT-004 F10): see text_rules.h. */
#include "text_rules.h"

#include <string.h>

const char *const TEXT_TITLES[15] = { "Mr", "Mrs", "Ms", "Dr", "St", "Capt", "Col", "Lt", "Gen", "Prof", "Sgt", "Jr", "Sr",
                                      "Rev", "Hon" };
const char *const TEXT_TITLES_QUESTION[5] = { "Mr", "Mrs", "Ms", "Dr", "St" };
const char *const TEXT_TITLES_EXCERPT[8] = { "mr", "mrs", "ms", "dr", "st", "rev", "col", "capt" };
const char *const TEXT_NAME_HONORIFICS[6] = { "Mr.", "Mrs.", "Miss", "Dr.", "Sir", "Lady" };

const char *const TEXT_RELATION_WORDS[27] = { "father", "mother", "son", "daughter", "sister", "sisters", "brother",
                                              "brothers", "wife", "husband", "friend", "estate", "fortune", "inheritance",
                                              "property", "nephew", "niece", "cousin", "cousins", "uncle", "aunt",
                                              "grandfather", "grandmother", "grandson", "granddaughter", "widow", "heir" };
const char *const TEXT_OBSERVER_KIN_WORDS[29] = { "father", "mother", "son", "daughter", "brother", "sister", "aunt", "uncle",
    "niece", "nephew", "cousin", "grandfather", "grandmother", "granddaughter", "grandson", "wife", "husband", "widow",
    "widower", "parent", "parents", "child", "children", "sons", "daughters", "brothers", "sisters", "grandparent", NULL };

const char *const TEXT_NUMBER_WORDS[21] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
                                            "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                            "seventeen", "eighteen", "nineteen", "twenty" };
const char *const TEXT_TENS_WORDS[8] = { "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety" };
const char *const TEXT_OBSERVER_NUMBER_WORDS[39] = { "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
    "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "thousand", "first", "second", "third",
    "fourth", "fifth", "eldest", "youngest", "twice", "once", NULL };

bool text_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool text_word_boundary(const char *s, size_t i) { return i == 0 || !text_word_char(s[i - 1]); }

bool text_is_initial(const char *w, size_t n) { return n == 1 && w[0] >= 'A' && w[0] <= 'Z'; }

bool text_title_before(const char *s, size_t dot, const char *const *titles, size_t ntitles)
{
    for (size_t k = 0; k < ntitles; k++) {
        size_t n = strlen(titles[k]);
        if (dot >= n && !strncmp(s + dot - n, titles[k], n) && text_word_boundary(s, dot - n)) return true;
    }
    return false;
}

bool text_abbreviation_before(const char *s, size_t dot)
{
    return (dot >= 1 && text_is_initial(s + dot - 1, 1) && text_word_boundary(s, dot - 1)) ||
           text_title_before(s, dot, TEXT_TITLES, TEXT_COUNT(TEXT_TITLES));
}

bool text_word_in_folded(const char *w, size_t n, const char *const *words, size_t nwords)
{
    for (size_t k = 0; k < nwords; k++) {
        if (strlen(words[k]) != n) continue;
        size_t i = 0;
        for (; i < n; i++) {
            char c = w[i] >= 'A' && w[i] <= 'Z' ? (char)(w[i] + ('a' - 'A')) : w[i];
            if (c != words[k][i]) break;
        }
        if (i == n) return true;
    }
    return false;
}
