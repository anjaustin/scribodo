/* The scripted host's completeness check (app/script.h, AUDIT-004 F3): every member is named, and a member left
 * NULL is reported by name. Headless: only the check is linked. */
#include <string.h>

#include "app/script.h"
#include "test_util.h"

static void test_complete_host(void)
{
    ScriptHost h;
    memset(&h, 0x01, sizeof(h)); /* every member non-NULL */
    CHECK(script_host_missing(&h) == NULL);
}

/* Each pointer-sized slot of the struct is checked, under a distinct name: the list covers the whole struct. */
static void test_each_member_detected(void)
{
    const char *names[SCRIPT_HOST_MEMBERS] = { 0 };
    for (size_t i = 0; i < SCRIPT_HOST_MEMBERS; i++) {
        ScriptHost h;
        memset(&h, 0x01, sizeof(h));
        memset((unsigned char *)&h + i * sizeof(void *), 0, sizeof(void *));
        names[i] = script_host_missing(&h);
        CHECK(names[i] != NULL);
        for (size_t j = 0; names[i] && j < i; j++) CHECK(names[j] && strcmp(names[i], names[j]) != 0);
    }
    CHECK(names[0] && !strcmp(names[0], "editor"));
    CHECK(names[SCRIPT_HOST_MEMBERS - 1] && !strcmp(names[SCRIPT_HOST_MEMBERS - 1], "model_status"));
    ScriptHost h;
    memset(&h, 0x01, sizeof(h));
    h.recall_busy = NULL;
    CHECK(script_host_missing(&h) && !strcmp(script_host_missing(&h), "recall_busy"));
    h.ctx = NULL;
    CHECK(script_host_missing(&h) && !strcmp(script_host_missing(&h), "ctx")); /* the first missing one */
}

int main(void)
{
    TestCase tests[] = {
        { "complete_host", test_complete_host },
        { "each_member_detected", test_each_member_detected },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
