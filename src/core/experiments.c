#include "experiments.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static Experiments g_experiments;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static bool flag_is(const char *name, const char *value)
{
    const char *v = getenv(name);
    return v && !strcmp(v, value);
}

static void read_environment(void)
{
    Experiments e = {
        .answer_parts = flag_is("SCRIBODO_ANSWER_PARTS", "1"),
        .answer_block_in_message = flag_is("SCRIBODO_ANSWER_BLOCK", "message"),
        .memory_trace = flag_is("SCRIBODO_MEMORY_TRACE", "1"),
        .memory_excerpts = flag_is("SCRIBODO_MEMORY_EXCERPTS", "1"),
        .memory_small_units = flag_is("SCRIBODO_MEMORY_SMALL_UNITS", "1"),
        .semantic_debug = getenv("SCRIBODO_SEM_DEBUG") != NULL,
        .memory_experiment = flag_is("SCRIBODO_MEMORY_EXPERIMENT", "1"),
        .memory_briefs = flag_is("SCRIBODO_MEMORY_BRIEFS", "1"),
        .answer_pipeline = flag_is("SCRIBODO_ANSWER_PIPELINE", "verified"),
        .memory_presentation_compact = flag_is("SCRIBODO_MEMORY_PRESENTATION", "compact"),
        .memory_presentation_episode = flag_is("SCRIBODO_MEMORY_PRESENTATION", "episode"),
    };
    g_experiments = e;
}

const Experiments *experiments(void)
{
    pthread_once(&g_once, read_environment);
    return &g_experiments;
}

void experiments_reload(void)
{
    read_environment();
    /* After an explicit reload the once-flag is spent either way; the table is the current environment. */
    pthread_once(&g_once, read_environment);
}
