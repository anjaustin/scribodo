/* Shared text rules: word characters, the titles and initials whose period ends no sentence, the relation and
 * kinship words and the number words (AUDIT-004 F10).
 *
 * The lists were copied file to file and drifted: the relation check's splitter knew four titles and no initials
 * while the observer's knew fifteen and initials, so "Ms. Bennet" and "James K. Powell" were cut mid-name. One home
 * now. Where a module's measured behaviour rests on a particular list (a frozen vocabulary, a Python mirror pinned by
 * a fixture), that exact list is kept here as its own named constant rather than merged into another: changing a
 * list is a behaviour change and is measured like one (journal/audit004-text.md).
 *
 * Adopting a list: replace the module's own copy with the constant named for it below; byte-identical behaviour is
 * the bar, proved by the module's tests and a differential over recorded text. The observer's lists
 * (src/llm/observer_text.c) are here under TEXT_OBSERVER_* for that stream to adopt; until it does, they are copies.
 *
 * Pure: ASCII rules, no allocation, no locale, no I/O. */
#ifndef SCRIBODO_TEXT_RULES_H
#define SCRIBODO_TEXT_RULES_H

#include <stdbool.h>
#include <stddef.h>

#define TEXT_COUNT(list) (sizeof(list) / sizeof((list)[0]))

/* ---- word characters ---------------------------------------------------------------------------------------- */

/* An ASCII letter, digit or underscore: the regular expressions' \w on ASCII (bytes of UTF-8 sequences are not). */
bool text_word_char(char c);
/* Position i of s starts a word: i is 0 or the byte before it is no word character (the regular expressions' \b
 * before a word). */
bool text_word_boundary(const char *s, size_t i);

/* ---- titles and initials ---------------------------------------------------------------------------------------- */

/* The titles whose period ends no sentence ("Mr. Bennet", "Capt. Kidd"): tools/observer_plant.py ABBREVIATIONS, the
 * observer's splitters (obs_first_sentence case-sensitive, obs_sentence_end case-folded) and, since AUDIT-004 F10,
 * the relation check's (tools/answer_relation_arm.py TITLES, pinned by tests/fixtures/answer_relation). */
extern const char *const TEXT_TITLES[15];
/* The question splitter's titles (answer_pipeline.c sentences(), question parts; journal/answer-cycle3_*.md): kept
 * as measured. */
extern const char *const TEXT_TITLES_QUESTION[5];
/* The context planner's excerpt splitter: lower case, compared case-folded (context_planner.c document_excerpts). */
extern const char *const TEXT_TITLES_EXCERPT[8];
/* The honorifics a relation-check name may open with, period included where it has one: the arm's NAME pattern
 * (tools/answer_relation_arm.py), pinned by tests/fixtures/answer_relation. A name prefix, not a splitting rule. */
extern const char *const TEXT_NAME_HONORIFICS[6];

/* w[0..n) is one ASCII capital (the shape of an initial; whether it is one depends on its neighbours, below). */
bool text_is_initial(const char *w, size_t n);
/* The words before a capital and its period that make the capital a label, not an initial ("Chapter V.", "Part I.",
 * "Source B."), and the words after one that start a sentence, not a surname ("... Plan B. That is ..."). */
extern const char *const TEXT_LABEL_WORDS[30];
extern const char *const TEXT_SENTENCE_STARTERS[47];
/* The period at s[dot] (s has n bytes) closes a name's initial ("James K. Powell"): s[dot - 1] is one capital other
 * than the pronoun "I", at a word boundary; after the period and whitespace comes a capitalised word that is not a
 * TEXT_SENTENCE_STARTERS word; before the capital, after spaces or tabs, is a capitalised word (leading quotes and
 * brackets ignored, one trailing period allowed, so "J. K." chains) that is not a TEXT_LABEL_WORDS word. Any capital
 * with a period ended no sentence before the AUDIT-004 red team: "... in Chapter V. Lydia is not his daughter."
 * joined two sentences and the negation hid the first's wrong relation. */
bool text_name_initial_before(const char *s, size_t n, size_t dot);
/* s[0..dot) ends with one of `titles` (case-sensitive) that starts at a word boundary. */
bool text_title_before(const char *s, size_t dot, const char *const *titles, size_t ntitles);
/* The sentence rule: the period at s[dot] closes a TEXT_TITLES title at a word boundary, or a name's initial
 * (text_name_initial_before). Mirrored by tools/answer_relation_arm.py (abbreviation_at). */
bool text_abbreviation_before(const char *s, size_t n, size_t dot);
/* w[0..n) equals one of the lower-case `words` with ASCII letters folded to lower case. */
bool text_word_in_folded(const char *w, size_t n, const char *const *words, size_t nwords);

/* ---- relation and kinship words ----------------------------------------------------------------------------------- */

/* The relation check's closed list, kinship and possession together (answer_pipeline.c answer_relations; the arm's
 * KIN, pinned by tests/fixtures/answer_relation). */
extern const char *const TEXT_RELATION_WORDS[27];
/* The observer's kinship vocabulary, frozen 2026-11-14 with tools/observer_judge_score.py (observer_text.c
 * k_kin_words), NULL-terminated as there. */
extern const char *const TEXT_OBSERVER_KIN_WORDS[29];

/* ---- number words ----------------------------------------------------------------------------------------------- */

/* "zero" to "twenty", index = value. */
extern const char *const TEXT_NUMBER_WORDS[21];
/* "twenty" to "ninety", index = tens digit - 2. */
extern const char *const TEXT_TENS_WORDS[8];
/* The observer's number vocabulary, frozen 2026-11-14 with tools/observer_judge_score.py (observer_text.c
 * k_number_words: cardinals, scale words, ordinals, "twice", "once"), NULL-terminated as there. */
extern const char *const TEXT_OBSERVER_NUMBER_WORDS[39];

#endif
