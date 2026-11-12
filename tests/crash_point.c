/* The kill-point implementation for test binaries built with -DSCRIBODO_CRASH_POINTS (core/crash_point.h).
 * SCRIBODO_CRASH_POINT is read once, at load, so a hit from any thread sees the armed point without a race. */
#include "core/crash_point.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_point[128];
static long g_target = 0; /* 0: no point selected */
static atomic_long g_hits;

__attribute__((constructor)) static void crash_point_arm(void)
{
    const char *spec = getenv("SCRIBODO_CRASH_POINT");
    if (!spec || !*spec) return;
    const char *hash = strchr(spec, '#');
    size_t n = hash ? (size_t)(hash - spec) : strlen(spec);
    if (!n || n >= sizeof(g_point)) return;
    memcpy(g_point, spec, n);
    g_point[n] = '\0';
    long target = hash ? strtol(hash + 1, NULL, 10) : 1;
    g_target = target >= 1 ? target : 1;
}

void crash_point(const char *name)
{
    if (!g_target || !name || strcmp(name, g_point)) return;
    long hit = atomic_fetch_add(&g_hits, 1) + 1;
    if (hit != g_target) return;
    fprintf(stderr, "CRASH_POINT %s#%ld\n", name, hit);
    kill(getpid(), SIGKILL);
    _exit(137);
}

long crash_point_hits(void) { return g_target ? atomic_load(&g_hits) : 0; }
