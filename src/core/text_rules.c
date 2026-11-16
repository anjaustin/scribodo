/* Shared text rules (AUDIT-004 F10): see text_rules.h. */
#include "text_rules.h"

#include <string.h>

const char *const TEXT_TITLES[15] = { "Mr", "Mrs", "Ms", "Dr", "St", "Capt", "Col", "Lt", "Gen", "Prof", "Sgt", "Jr", "Sr",
                                      "Rev", "Hon" };
const char *const TEXT_TITLES_QUESTION[5] = { "Mr", "Mrs", "Ms", "Dr", "St" };
const char *const TEXT_TITLES_EXCERPT[8] = { "mr", "mrs", "ms", "dr", "st", "rev", "col", "capt" };
/* The observer's lists (AUDIT-004 F10 wave 2): moved here from observer_text.c word for word. */
const char *const TEXT_LABEL_WORDS[30] = { "Chapter", "Ch", "Part", "Book", "Volume", "Vol", "Act", "Scene", "Section",
    "Sec", "Source", "Appendix", "Figure", "Fig", "Table", "Plate", "Page", "Item", "Note", "Step", "Letter", "Line",
    "Option", "Plan", "Type", "Class", "Group", "Exhibit", "Article", "Clause" };
const char *const TEXT_SENTENCE_STARTERS[47] = { "The", "This", "That", "These", "Those", "It", "Its", "He", "She", "They",
    "We", "You", "I", "His", "Her", "Their", "My", "Our", "There", "Here", "Then", "Thus", "So", "But", "And", "Or", "If",
    "When", "While", "As", "In", "On", "At", "For", "With", "From", "According", "However", "Also", "Yes", "No", "Now",
    "Who", "What", "Which", "Where", "A" };
const char *const TEXT_OBSERVER_TITLE_WORDS[42] = {
        "mr", "mrs", "ms", "miss", "mister", "master", "lady", "lord", "sir", "dame", "madam", "madame", "dr",
        "doctor", "captain", "capt", "colonel", "col", "major", "general", "gen", "lieutenant", "lt", "sergeant",
        "sgt", "reverend", "rev", "professor", "prof", "king", "queen", "prince", "princess", "duke", "duchess",
        "count", "countess", "baron", "baroness", "earl", "saint", "st"
};
const char *const TEXT_OBSERVER_COLOR_WORDS[22] = { "black", "white", "red", "blue", "green", "brown", "golden", "gold", "grey",
    "gray", "yellow", "crimson", "scarlet", "silver", "dark", "fair", "pale", "auburn", "copper", "purple", "violet", NULL };
const char *const TEXT_OBSERVER_TIME_WORDS[28] = { "january", "february", "march", "april", "may", "june", "july", "august",
    "september", "october", "november", "december", "before", "after", "earlier", "later", "years", "year", "days", "day",
    "months", "month", "weeks", "week", "age", "aged", "old", NULL };
const char *const TEXT_OBSERVER_PLACE_TITLES[26] = { "mr", "mrs", "ms", "miss", "lady", "lord", "sir", "dr", "captain", "colonel",
    "prince", "princess", "king", "queen", "the", "a", "an", "he", "she", "it", "they", "his", "her", "canon", "new", NULL };
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

bool text_ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static bool in_exact(const char *const *list, size_t count, const char *w, size_t n)
{
    for (size_t i = 0; i < count; i++)
        if (strlen(list[i]) == n && !strncmp(list[i], w, n)) return true;
    return false;
}

/* Moved from observer_text.c (name_initial) unchanged but for the names of its helpers. */
bool text_name_initial_in_context(const char *s, size_t n, size_t dot)
{
    if (dot < 1 || dot >= n || s[dot - 1] < 'A' || s[dot - 1] > 'Z' || s[dot - 1] == 'I') return false;
    if (dot >= 2 && text_word_char(s[dot - 2])) return false;
    size_t j = dot + 1; /* after: whitespace, then a capitalised word that does not start a sentence */
    while (j < n && text_ascii_space(s[j])) j++;
    if (j >= n || s[j] < 'A' || s[j] > 'Z') return false;
    size_t je = j;
    while (je < n && text_ascii_letter(s[je])) je++;
    if (in_exact(TEXT_SENTENCE_STARTERS, TEXT_COUNT(TEXT_SENTENCE_STARTERS), s + j, je - j)) return false;
    size_t e = dot - 1; /* before: past what is attached to the capital (an opening quote), spaces, then a word */
    while (e > 0 && !text_ascii_space(s[e - 1])) e--;
    while (e > 0 && text_ascii_space(s[e - 1])) e--;
    size_t b = e;
    while (b > 0 && !text_ascii_space(s[b - 1])) b--;
    while (b < e && !text_ascii_letter(s[b])) b++;
    while (e > b && !text_ascii_letter(s[e - 1])) e--;
    return e <= b || !in_exact(TEXT_LABEL_WORDS, TEXT_COUNT(TEXT_LABEL_WORDS), s + b, e - b);
}

bool text_ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

bool text_abbreviation_before(const char *s, size_t n, size_t dot)
{
    (void)n;
    return text_title_before(s, dot, TEXT_TITLES, TEXT_COUNT(TEXT_TITLES)) ||
           (dot >= 1 && text_is_initial(s + dot - 1, 1) && text_word_boundary(s, dot - 1));
}

bool text_initial_period(const char *s, size_t n, size_t p)
{
    return p >= 1 && p + 1 < n && s[p] == '.' && text_is_initial(s + p - 1, 1) && text_word_boundary(s, p - 1) &&
           text_ascii_space(s[p + 1]);
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
