#define _DARWIN_C_SOURCE
#include "test_state_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool test_state_fixture_init(char *directory, size_t length, StoreManager **store)
{
    const char *base = getenv("TMPDIR");
    int n = snprintf(directory, length, "%s/scribodo-stateful-XXXXXX", base && *base ? base : "/tmp");
    if (n < 0 || (size_t)n >= length || !mkdtemp(directory)) return false;
    *store = store_manager_create();
    if (*store) return true;
    test_state_fixture_destroy(directory, NULL);
    return false;
}

void test_state_fixture_destroy(const char *directory, StoreManager *store)
{
    store_manager_destroy(store);
    if (!directory || !directory[0]) return;
    char command[1200];
    int n = snprintf(command, sizeof(command), "rm -rf '%s'", directory);
    if (n >= 0 && (size_t)n < sizeof(command) && system(command) != 0)
        fprintf(stderr, "warning: could not remove %s\n", directory);
}
