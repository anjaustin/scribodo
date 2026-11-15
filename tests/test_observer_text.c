#include <stdlib.h>
#include <string.h>

#include "core/experiments.h"
#include "core/utf8.h"
#include <stdint.h>
#include "llm/observer.h"
#include "test_util.h"

static bool locate_is(const char *text, const char *evidence, const char *want)
{
    size_t start = 0, len = 0;
    bool ok = obs_locate(text, strlen(text), evidence, &start, &len);
    if (!want) return !ok;
    bool matches = ok && len == strlen(want) && !strncmp(text + start, want, len);
    if (!matches) fprintf(stderr, "    locate [%s] got [%.*s]\n", evidence, ok ? (int)len : 0, ok ? text + start : "");
    return matches;
}

static void test_locate(void)
{
    const char *text = "\xE2\x80\x9CYou\xE2\x80\x99re late,\xE2\x80\x9D said Dvo\xC5\x99\xC3\xA1k, without turning around.\n"
                       "He grunted \xE2\x80\x94 the *particular* grunt that meant\nwe will talk about this later.";
    CHECK(locate_is(text, "without turning around", "without turning around"));
    CHECK(locate_is(text, "\"You're late,\" said Dvo\xC5\x99\xC3\xA1k", "\xE2\x80\x9CYou\xE2\x80\x99re late,\xE2\x80\x9D said Dvo\xC5\x99\xC3\xA1k"));
    CHECK(locate_is(text, "the particular grunt that meant we will talk", "the *particular* grunt that meant\nwe will talk"));
    CHECK(locate_is(text, "He grunted \xE2\x80\x94 the particular grunt that meant trouble ahead", "He grunted \xE2\x80\x94 the *particular* grunt that meant"));
    CHECK(locate_is(text, "Dvo\xC5\x99\xC3\xA1k smiled warmly at her", NULL));
    CHECK(locate_is(text, "late", "late"));
    CHECK(locate_is(text, "", NULL));
    CHECK(locate_is(text, "WITHOUT turning around", "without turning around"));
    CHECK(locate_is("Zo\xC3\xAB climbed the stairs.", "ed", NULL));
    CHECK(locate_is("Zo\xC3\xAB climbed the stairs.", "imbed the", NULL));
    CHECK(locate_is("Mara left the lighthouse at dawn.", "Mara left the lighthouse and then burned it to the ground with Zo\xC3\xAB inside", NULL));
    CHECK(locate_is("Mara left the lighthouse at dawn.", "Mara left the lighthouse at dusk", "Mara left the lighthouse at"));
}

static void test_paragraphs_and_overlap(void)
{
    size_t starts[8], lens[8];
    const char *document = "\n\nFirst para\nstill first.\n\n  \n\nSecond.\n\n\n";
    size_t count = obs_paragraphs(document, starts, lens, 8);
    CHECK(count == 2 && !strncmp(document + starts[0], "First para\nstill first.", lens[0]) && lens[0] == 23 &&
          !strncmp(document + starts[1], "Second.", lens[1]));
    CHECK(obs_overlap("Zo\xC3\xAB\xE2\x80\x99s eyes are brown.", "Zo\xC3\xAB's eyes are green.") >= 3);
    CHECK(obs_overlap("The fog came in.", "Dvo\xC5\x99\xC3\xA1k lost his hand.") == 0);
    /* a shared title counts as a shared name by default (measured: the stoplist lost a place fault whose only shared
     * word was "Princess"); under the conflict-gate experiment it does not (2026-11-14) */
    unsetenv("SCRIBODO_OBSERVER_CONFLICT_GATE");
    experiments_reload();
    CHECK(obs_overlap("Mr. Darcy's estate is in Derbyshire.", "Mr. Wickham joined the Regulars.") == 2);
    CHECK(obs_overlap("Dejah Thoris is the Princess of Helium.", "She was, after all, the Princess of Zodanga.") == 2);
    setenv("SCRIBODO_OBSERVER_CONFLICT_GATE", "1", 1);
    experiments_reload();
    CHECK(obs_overlap("Mr. Darcy's estate is in Derbyshire.", "Mr. Wickham joined the Regulars.") == 0);
    CHECK(obs_overlap("Lady Catherine de Bourgh is Mr. Darcy's aunt.", "Lady Lucas called at Longbourn.") == 0);
    CHECK(obs_overlap("Captain James K. Powell is of Richmond.", "Powell had been born in Boston.") == 2);
    CHECK(obs_overlap("Dejah Thoris is the Princess of Helium.", "She was, after all, the Princess of Zodanga.") == 0);
    unsetenv("SCRIBODO_OBSERVER_CONFLICT_GATE");
    experiments_reload();
    CHECK(obs_overlap("Mr. Darcy's estate is in Derbyshire.", "Mr. Wickham joined the Regulars.") == 2);
    CHECK(obs_title("Mrs", 3) && obs_title("captain", 7) && !obs_title("Darcy", 5) && !obs_title("Mrsx", 4));
}

static bool valid_utf8(const char *s)
{
    size_t n = strlen(s), invalid = 0;
    uint32_t *cps = malloc((n + 1) * sizeof(uint32_t));
    if (!cps) return false;
    utf8_decode((const uint8_t *)s, n, cps, &invalid);
    free(cps);
    return invalid == 0;
}

static void test_statement_clips_by_characters(void)
{
    /* 298 ASCII bytes, then an em dash (3 bytes, straddling byte 300), then more: the old "%.300s" cut inside the
     * dash. 305 characters, so the clip at 300 keeps the whole dash and one space and drops "more.". */
    char quote[400];
    memset(quote, 'a', 298);
    memcpy(quote + 298, "\xE2\x80\x94 more.", 9);
    quote[307] = '\0';
    char out[OBS_STATEMENT_BYTES];
    obs_statement("Zo\xC3\xAB's eyes are brown.", quote, out, sizeof(out));
    CHECK(valid_utf8(out));
    CHECK(strstr(out, "a\xE2\x80\x94 \xE2\x80\x9D)") != NULL && !strstr(out, "more."));
    memcpy(quote + 290, "\xE2\x80\x94 more words.", 15); /* 305 bytes, 303 characters: three characters clipped */
    quote[305] = '\0';
    obs_statement("d", quote, out, sizeof(out));
    CHECK(valid_utf8(out) && strstr(out, "\xE2\x80\x94 more wor\xE2\x80\x9D)") != NULL);
    char longq[1300];
    for (size_t i = 0; i < 400; i++) memcpy(longq + 3 * i, "\xE2\x80\x94", 3); /* 400 em dashes: 1,200 bytes */
    longq[1200] = '\0';
    obs_statement("d", longq, out, sizeof(out));
    CHECK(valid_utf8(out) && strlen(out) == 1 + 2 + 3 + 300 * 3 + 3 + 1); /* 300 characters kept */
    obs_statement(NULL, NULL, out, sizeof(out));
    CHECK(!strcmp(out, " (\xE2\x80\x9C\xE2\x80\x9D)"));
}

static void test_values_in_texts(void)
{
    const char *canon = "Dejah Thoris is the daughter of Mors Kajak and the granddaughter of Tardos Mors.";
    const char *stmt = "Dejah Thoris was the daughter of Tardos Mors himself.";
    CHECK(obs_values_in_texts(canon, stmt, "daughter of Mors Kajak", "daughter of Tardos Mors"));
    CHECK(!obs_values_in_texts(canon, stmt, "daughter of Mors Kajak", "daughter of Mors Kajak")); /* the same value */
    CHECK(!obs_values_in_texts(canon, stmt, "", "daughter of Tardos Mors"));                     /* nothing named */
    CHECK(!obs_values_in_texts(canon, stmt, "wears silks", "daughter of Tardos Mors"));           /* not in canon */
    CHECK(!obs_values_in_texts(canon, stmt, "daughter of Mors Kajak", "Princess of Helium"));     /* not in new */
    CHECK(!obs_values_in_texts(canon, NULL, "a", "b") && !obs_values_in_texts(canon, stmt, NULL, "b"));
    /* quote style and emphasis do not defeat the location */
    CHECK(obs_values_in_texts("Zo\xC3\xAB\xE2\x80\x99s eyes are green.", "Zo\xC3\xAB's eyes are *brown*.", "Zo\xC3\xAB's eyes are green", "eyes are brown"));
}

static bool first_sentence_is(const char *text, const char *want)
{
    char buf[400];
    snprintf(buf, sizeof(buf), "%s", text);
    obs_first_sentence(buf);
    if (strcmp(buf, want)) fprintf(stderr, "    first sentence of [%s] got [%s]\n", text, buf);
    return !strcmp(buf, want);
}

static void test_first_sentence(void)
{
    CHECK(first_sentence_is("Canon states Captain James K. Powell is of Richmond. The new text says Boston.",
                            "Canon states Captain James K. Powell is of Richmond."));
    CHECK(first_sentence_is("Mr. Darcy is tall. He is rich.", "Mr. Darcy is tall."));
    CHECK(first_sentence_is("Blue. Then red.", "Blue."));
    CHECK(first_sentence_is("Powell left on March 3. The text says May.", "Powell left on March 3."));
    CHECK(first_sentence_is("Her eyes are blue. the text says brown.", "Her eyes are blue. the text says brown."));
    CHECK(first_sentence_is("No period at all", "No period at all"));
    CHECK(first_sentence_is("It ends with K.", "It ends with K."));
    CHECK(first_sentence_is("Capt. Powell rode. Then he fell.", "Capt. Powell rode."));
    CHECK(first_sentence_is("The new text introduces 'Mrs. Collins' as a friend. Then more.",
                            "The new text introduces 'Mrs. Collins' as a friend."));
    CHECK(first_sentence_is("A person named (Mr. Gardiner) writes. Then more.", "A person named (Mr. Gardiner) writes."));
    CHECK(first_sentence_is("It names **Mr. Darcy** of Pemberley. Then more.", "It names **Mr. Darcy** of Pemberley."));
}

static void test_sentence_end(void)
{
    const char *s = "He met Mr. Darcy. Then J. K. Rowling wrote. It was 1866. So.";
    size_t e = obs_sentence_end(s, strlen(s));
    CHECK(e == strlen("He met Mr. Darcy."));
    const char *t = s + e + 1;
    CHECK(obs_sentence_end(t, strlen(t)) == strlen("Then J. K. Rowling wrote."));
    const char *q = "\xE2\x80\x9CMr. Bingley!\xE2\x80\x9D she cried. Next";              /* "she": no boundary */
    CHECK(obs_sentence_end(q, strlen(q)) == strlen("\xE2\x80\x9CMr. Bingley!\xE2\x80\x9D she cried."));
    const char *r2 = "\xE2\x80\x9CMr. Bingley!\xE2\x80\x9D She cried.";
    CHECK(obs_sentence_end(r2, strlen(r2)) == strlen("\xE2\x80\x9CMr. Bingley!\xE2\x80\x9D"));
    const char *k = "my machine and Kantos Kan\xE2\x80\x99s. Making his way";
    CHECK(obs_sentence_end(k, strlen(k)) == strlen("my machine and Kantos Kan\xE2\x80\x99s."));
    const char *d = "drily--\xE2\x80\x9CMr. Darcy is kind.\xE2\x80\x9D Yes.";
    CHECK(obs_sentence_end(d, strlen(d)) == strlen("drily--\xE2\x80\x9CMr. Darcy is kind.\xE2\x80\x9D"));
    CHECK(obs_sentence_end("one line\ntwo", 12) == 8);         /* a line break ends one */
    CHECK(obs_sentence_end("ends with i. next", 17) == 17);    /* a lower-case word after: not a boundary */
    CHECK(obs_sentence_end("no end", 6) == 6);
}

static void test_attributes_differ(void)
{
    CHECK(obs_attributes_differ("Kantos Kan is a padwar in the navy of Helium.", "a padwar in the navy of Zodanga"));
    CHECK(!obs_attributes_differ("Dejah Thoris is the Princess of Helium.", "She smiled at the Princess."));      /* one side only */
    CHECK(obs_attributes_differ("Sola is the daughter of Tars Tarkas.", "Sola was the sister of Lorquas."));
    CHECK(obs_attributes_differ("thirty days after", "only three days after"));
    CHECK(obs_attributes_differ("Her hair is coal black.", "Her golden hair fell."));
    CHECK(!obs_attributes_differ("Her hair is black.", "Her black hair fell."));                                 /* same value */
    CHECK(!obs_attributes_differ("The Princess of Helium.", "She went to Mr Darcy."));                              /* a title is no place */
    CHECK(obs_attributes_differ("married three-and-twenty years", "married above forty years"));
    CHECK(!obs_attributes_differ("", "anything at all 12"));
}

static void test_same_subject(void)
{
    CHECK(obs_same_subject("Kantos Kan is a padwar in the navy of Helium.", "Kantos Kan was a padwar of Zodanga."));
    CHECK(obs_same_subject("Sola's father is Tars Tarkas.", "It was then that Sola's mother spoke."));    /* a possessive */
    CHECK(obs_same_subject("Kantos Kan \xE2\x80\x94 a padwar of Helium.", "Kantos Kan rose."));          /* before the dash */
    CHECK(!obs_same_subject("Powell left for the valley on March 3, 1866.", "It had been the third of May when Powell rode."));
    CHECK(!obs_same_subject("Mr. Darcy is tall.", "Mr. Bingley is tall."));                                   /* a title alone */
    CHECK(!obs_same_subject("Mr. Darcy is tall.", "He said Darcy was tall."));                                /* not leading */
    CHECK(obs_same_subject("Mr. Darcy is tall.", "\xE2\x80\x9C" "Darcy is short,\xE2\x80\x9D she said."));       /* quotes skipped */
    CHECK(obs_same_subject("Sola is here.", "\xE2\x80\x9C" "Sola's eyes,\xE2\x80\x9D he said."));
    /* Known limit of the frozen rule: ASCII names only, so an accented name never co-signs (a missed disclosure,
     * never a false one). */
    CHECK(!obs_same_subject("Zo\xC3\xAB's eyes are green.", "Zo\xC3\xAB's eyes are brown."));
    CHECK(!obs_same_subject("", "Sola is here."));
}

int main(void)
{
    const TestCase tests[] = {
        { "locate", test_locate },
        { "paragraphs_and_overlap", test_paragraphs_and_overlap },
        { "first_sentence", test_first_sentence },
        { "values_in_texts", test_values_in_texts },
        { "statement_clips_by_characters", test_statement_clips_by_characters },
        { "sentence_end", test_sentence_end },
        { "attributes_differ", test_attributes_differ },
        { "same_subject", test_same_subject },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
