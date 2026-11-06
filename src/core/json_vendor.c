/* Track allocation failure without changing cJSON's hooks at runtime or
 * modifying the pinned checkout. The parser boundary reads this thread's flag. */
#include <stdbool.h>
#include <stdlib.h>

static _Thread_local bool allocation_failed;

static void *tracked_malloc(size_t n)
{
    void *p = malloc(n);
    if (!p) allocation_failed = true;
    return p;
}

static void *tracked_realloc(void *p, size_t n)
{
    void *next = realloc(p, n);
    if (!next && n) allocation_failed = true;
    return next;
}

void json_vendor_reset_allocation_error(void) { allocation_failed = false; }
bool json_vendor_allocation_error(void) { return allocation_failed; }

#undef malloc
#undef realloc
#define malloc tracked_malloc
#define realloc tracked_realloc
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "../../third_party/cJSON/cJSON.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
