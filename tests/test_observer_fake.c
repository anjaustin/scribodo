#include <stdlib.h>
#include <string.h>

#include "llm/observer.h"
#include "test_util.h"

static void test_extract_and_judge(void)
{
    char *result = obs_fake_extract("The fog came. Zo\xC3\xAB's eyes are brown. Is Dvo\xC5\x99\xC3\xA1k her uncle?");
    CHECK(result && strstr(result, "\"question\"") && strstr(result, "\"event\"") && strstr(result, "Dvo\xC5\x99\xC3\xA1k is mentioned"));
    free(result);

    result = obs_fake_judge("Zo\xC3\xAB's eyes are green.", "Zo\xC3\xAB's eyes are brown.");
    CHECK(result && strstr(result, "\"conflict\""));
    free(result);

    result = obs_fake_judge("Zo\xC3\xAB's eyes are green.", "Zo\xC3\xAB's eyes are green.");
    CHECK(result && !strstr(result, "\"conflict\""));
    free(result);
}

int main(void)
{
    const TestCase tests[] = { { "extract_and_judge", test_extract_and_judge } };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
