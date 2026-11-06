#include <stdlib.h>
#include <string.h>

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
}

int main(void)
{
    const TestCase tests[] = {
        { "locate", test_locate },
        { "paragraphs_and_overlap", test_paragraphs_and_overlap },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
