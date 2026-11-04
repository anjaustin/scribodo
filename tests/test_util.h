/* Minimal test harness shared by the test binaries. */
#ifndef SCRIBODO_TEST_UTIL_H
#define SCRIBODO_TEST_UTIL_H

#include <stdint.h>
#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

typedef struct {
    const char *name;
    void (*fn)(void);
} TestCase;

static inline int run_tests(const TestCase *tests, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

/* xorshift64; deterministic across runs. */
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static inline uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)g_rng;
}

#endif
