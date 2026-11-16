#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "core/experiments.h"
#include "core/utf8.h"
#include <stdint.h>
#include "llm/observer.h"
#include "llm/observer_arms.h"
#include "test_util.h"
#include "cJSON.h"

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
    CHECK(obs_overlap("Zo\xC3\xAB\xE2\x80\x99s eyes are brown.", "Zo\xC3\xAB's eyes are green.", false) >= 3);
    CHECK(obs_overlap("The fog came in.", "Dvo\xC5\x99\xC3\xA1k lost his hand.", false) == 0);
    /* a shared title counts as a shared name by default (measured: the stoplist lost a place fault whose only shared
     * word was "Princess"); when the caller ignores titles (the canon candidate steps under the conflict gate,
     * 2026-11-14) it does not. The setting is a parameter: the text module reads no flag, whatever the environment
     * says (AUDIT-004 F12). */
    setenv("SCRIBODO_OBSERVER_CONFLICT_GATE", "1", 1);
    experiments_reload();
    CHECK(obs_overlap("Mr. Darcy's estate is in Derbyshire.", "Mr. Wickham joined the Regulars.", false) == 2);
    CHECK(obs_overlap("Dejah Thoris is the Princess of Helium.", "She was, after all, the Princess of Zodanga.", false) == 2);
    unsetenv("SCRIBODO_OBSERVER_CONFLICT_GATE");
    experiments_reload();
    CHECK(obs_overlap("Mr. Darcy's estate is in Derbyshire.", "Mr. Wickham joined the Regulars.", true) == 0);
    CHECK(obs_overlap("Lady Catherine de Bourgh is Mr. Darcy's aunt.", "Lady Lucas called at Longbourn.", true) == 0);
    CHECK(obs_overlap("Captain James K. Powell is of Richmond.", "Powell had been born in Boston.", true) == 2);
    CHECK(obs_overlap("Dejah Thoris is the Princess of Helium.", "She was, after all, the Princess of Zodanga.", true) == 0);
    CHECK(obs_overlap("Mr. Darcy's estate is in Derbyshire.", "Mr. Wickham joined the Regulars.", false) == 2);
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
    /* a single capital is an initial only in context (red-team 2026-11-15) */
    CHECK(first_sentence_is("Her father is Tardos Mors in Chapter V. Lydia is not his daughter.", "Her father is Tardos Mors in Chapter V."));
    CHECK(first_sentence_is("It is as stated in Part I. That is not in dispute.", "It is as stated in Part I."));
    CHECK(first_sentence_is("\"Proud,\" said I. Proud people breed sad sorrows.", "\"Proud,\" said I."));
    CHECK(first_sentence_is("Canon states Captain James K. Powell is of Richmond. Then more.", "Canon states Captain James K. Powell is of Richmond."));
    CHECK(first_sentence_is("Written by J. R. R. Tolkien. Then more.", "Written by J. R. R. Tolkien."));
    CHECK(first_sentence_is("T. S. Eliot wrote the poem. Then more.", "T. S. Eliot wrote the poem."));
    CHECK(first_sentence_is("The canon names E. Bennet as the eldest. Then more.", "The canon names E. Bennet as the eldest."));
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

/* tests/fixtures/observer_rules/cases.json, the cases tests/test_observer_rules.py also runs through the Python each
 * C rule was ported from (AUDIT-004 F2): the observer's sentence loop, the attribute co-signer and the subject
 * co-signer must each give every expected value. Run from the repository root. */
static char *read_fixture(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s (run from the repository root)\n", path); return NULL; }
    char *buf = NULL;
    size_t cap = 0, n = 0, got;
    do {
        if (n + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 16384;
            char *grown = realloc(buf, cap);
            if (!grown) { free(buf); fclose(f); return NULL; }
            buf = grown;
        }
        got = fread(buf + n, 1, 4096, f);
        n += got;
    } while (got > 0);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static const char *case_str(const cJSON *c, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(c, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void test_shared_rule_fixture(void)
{
    char *text = read_fixture("tests/fixtures/observer_rules/cases.json");
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    CHECK(root != NULL);
    free(text);
    if (!root) return;
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItem(root, "version")) == 1);
    size_t n_sent = 0, n_attr = 0, n_subj = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "sentences")) {
        const char *t = case_str(c, "text");
        const cJSON *want = cJSON_GetObjectItem(c, "want");
        CHECK(t && cJSON_IsArray(want));
        if (!t || !cJSON_IsArray(want)) continue;
        size_t pos = 0, start, len, k = 0, nwant = (size_t)cJSON_GetArraySize(want);
        bool same = true;
        while (obs_next_sentence(t, strlen(t), &pos, &start, &len)) {
            const char *w = k < nwant ? cJSON_GetArrayItem(want, (int)k)->valuestring : NULL;
            if (!w || strlen(w) != len || strncmp(w, t + start, len)) same = false;
            if (!same) fprintf(stderr, "    sentence %zu of [%s]: got [%.*s] want [%s]\n", k, t, (int)len, t + start, w ? w : "(none)");
            k++;
        }
        CHECK(same && k == nwant);
        n_sent++;
    }
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "attributes")) {
        const char *canon = case_str(c, "canon"), *t = case_str(c, "text");
        CHECK(canon && t && cJSON_IsBool(cJSON_GetObjectItem(c, "differ")));
        if (!canon || !t) continue;
        bool want = cJSON_IsTrue(cJSON_GetObjectItem(c, "differ")), got = obs_attributes_differ(canon, t);
        if (got != want) fprintf(stderr, "    attributes [%.60s] [%.60s]: got %d (%s)\n", canon, t, got, case_str(c, "why"));
        CHECK(got == want);
        n_attr++;
    }
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "subject")) {
        const char *canon = case_str(c, "canon"), *t = case_str(c, "text");
        CHECK(canon && t && cJSON_IsBool(cJSON_GetObjectItem(c, "same")));
        if (!canon || !t) continue;
        bool want = cJSON_IsTrue(cJSON_GetObjectItem(c, "same")), got = obs_same_subject(canon, t);
        if (got != want) fprintf(stderr, "    subject [%.60s] [%.60s]: got %d (%s)\n", canon, t, got, case_str(c, "why"));
        CHECK(got == want);
        n_subj++;
    }
    CHECK(n_sent >= 16 && n_attr >= 24 && n_subj >= 19);
    cJSON_Delete(root);
}

/* The co-signers' word sets are built in O(n log n): 100,000 distinct numbers and 100,000 possessive names a side take
 * milliseconds; the quadratic sets took 3.7 s for 40,000 numbers and would take tens of seconds here (red-team
 * 2026-11-15). The bound is generous for the sanitizer build and a loaded machine. */
static double secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_word_sets_bounded_time(void)
{
    const int n = 100000;
    char *a = malloc((size_t)n * 16 + 1), *b = malloc((size_t)n * 16 + 1), *c = malloc((size_t)n * 16 + 1), *d = malloc((size_t)n * 16 + 1);
    CHECK(a && b && c && d);
    if (!a || !b || !c || !d) { free(a); free(b); free(c); free(d); return; }
    char *pa = a, *pb = b, *pc = c, *pd = d;
    for (int i = 0; i < n; i++) {
        pa += sprintf(pa, "%d ", i);
        pb += sprintf(pb, "%d ", i + 1);
        int u = i, v = i + n;
        pc += sprintf(pc, "X%c%c%c%c's ", 'a' + u % 26, 'a' + u / 26 % 26, 'a' + u / 676 % 26, 'a' + u / 17576 % 26);
        pd += sprintf(pd, "X%c%c%c%c's ", 'a' + v % 26, 'a' + v / 26 % 26, 'a' + v / 676 % 26, 'a' + v / 17576 % 26);
    }
    double t0 = secs();
    CHECK(obs_attributes_differ(a, b));      /* 0..99999 against 1..100000 */
    CHECK(!obs_attributes_differ(a, a));
    CHECK(!obs_same_subject(c, d));          /* no name shared */
    CHECK(obs_same_subject(c, c));
    double took = secs() - t0;
    if (took >= 2.0) fprintf(stderr, "    word sets took %.2f s\n", took);
    CHECK(took < 2.0);
    free(a); free(b); free(c); free(d);
}

/* Every arm of the observer's flags has its measured figure (AUDIT-004 F5): each combination of the conflict gate and
 * the sentence levels has exactly one row, with a disclosure and a stated source; the default row's text is the one
 * shown since step 4; and the row the view reads follows the flags. */
static void test_observer_arm_table(void)
{
    size_t n = 0;
    const ObserverArm *arms = observer_arms(&n);
    const ObserverSentencesArm levels[] = { OBSERVER_SENTENCES_OFF, OBSERVER_SENTENCES_ATTRIBUTES, OBSERVER_SENTENCES_SUBJECT };
    CHECK(n == 2 * sizeof(levels) / sizeof(levels[0]));
    for (int gate = 0; gate < 2; gate++)
        for (size_t l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
            size_t rows = 0;
            for (size_t i = 0; i < n; i++)
                if (arms[i].conflict_gate == (gate == 1) && arms[i].sentences == levels[l]) {
                    rows++;
                    CHECK(arms[i].name && arms[i].name[0] && arms[i].measured && arms[i].measured[0]);
                    CHECK(arms[i].disclosure && !strncmp(arms[i].disclosure, "Possible conflicts are suggestions", 34) &&
                          strstr(arms[i].disclosure, "Each links to its source."));
                    CHECK(!!strstr(arms[i].measured, "step") != !strcmp(arms[i].measured, "not measured"));
                }
            CHECK(rows == 1);
        }
    unsetenv("SCRIBODO_OBSERVER_CONFLICT_GATE");
    unsetenv("SCRIBODO_OBSERVER_SENTENCES");
    experiments_reload();
    CHECK(!strcmp(observer_arm(experiments())->disclosure,
                  "Possible conflicts are suggestions: about 1 in 13 was real on the test novels. Each links to its source."));
    setenv("SCRIBODO_OBSERVER_SENTENCES", "attributes", 1);
    experiments_reload();
    CHECK(strstr(observer_arm(experiments())->measured, "step 8") != NULL);
    CHECK(strstr(observer_arm(experiments())->disclosure, "about 1 in 6 was real") != NULL); /* 11 of 60 = 0.183 */
    unsetenv("SCRIBODO_OBSERVER_SENTENCES");
    setenv("SCRIBODO_OBSERVER_CONFLICT_GATE", "1", 1);
    experiments_reload();
    CHECK(strstr(observer_arm(experiments())->measured, "time limit") != NULL); /* step 5's runs read part of each novel */
    setenv("SCRIBODO_OBSERVER_SENTENCES", "subject", 1);
    setenv("SCRIBODO_OBSERVER_CONFLICT_GATE", "1", 1);
    experiments_reload();
    CHECK(!strcmp(observer_arm(experiments())->measured, "not measured"));
    unsetenv("SCRIBODO_OBSERVER_CONFLICT_GATE");
    unsetenv("SCRIBODO_OBSERVER_SENTENCES");
    experiments_reload();
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
        { "shared_rule_fixture", test_shared_rule_fixture },
        { "observer_arm_table", test_observer_arm_table },
        { "word_sets_bounded_time", test_word_sets_bounded_time },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
