#define SCRIBODO_FAULT_ALLOC_IMPL
#include "fault_alloc.h"

static _Thread_local long remaining = -1;
static _Thread_local long calls;
static _Thread_local long failures;
static _Thread_local int single_failure;

void fault_alloc_after(long successful_calls) { remaining = successful_calls; calls = 0; failures = 0; single_failure = 0; }
void fault_alloc_once_after(long successful_calls) { remaining = successful_calls; calls = 0; failures = 0; single_failure = 1; }
long fault_alloc_count(void) { return calls; }
long fault_alloc_failures(void) { return failures; }

static int fail(void)
{
    calls++;
    if (remaining < 0) return 0;
    if (remaining == 0) {
        failures++;
        if (single_failure) remaining = -1;
        return 1;
    }
    remaining--;
    return 0;
}

void *fault_malloc(size_t n) { return fail() ? NULL : malloc(n); }
void *fault_calloc(size_t n, size_t size) { return fail() ? NULL : calloc(n, size); }
void *fault_realloc(void *p, size_t n) { return fail() ? NULL : realloc(p, n); }
char *fault_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = fault_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
