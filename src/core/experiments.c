#include "experiments.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Experiments g_experiments;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static char g_warnings[512]; /* the last read's warnings (experiments_warnings) */

static bool flag_is(const char *name, const char *value)
{
    const char *v = getenv(name);
    return v && !strcmp(v, value);
}

/* A default-on feature: off only when its variable is exactly "off" or "0" (a measured default that the author
 * can still switch off for a run or a measurement). */
static bool flag_on(const char *name)
{
    const char *v = getenv(name);
    return !(v && (!strcmp(v, "off") || !strcmp(v, "0")));
}

/* A test hook's count: the variable's integer value when positive, else 0. */
static int positive_int(const char *name)
{
    const char *v = getenv(name);
    int n = v ? atoi(v) : 0;
    return n > 0 ? n : 0;
}

/* A value of an observer flag that names no level: said on stderr and kept for experiments_warnings(), never guessed at
 * (red-team 2026-11-15: "Attributes", "on" or "attributes " silently ran the default arm). Values are matched exactly:
 * case-sensitive and untrimmed, like every flag in this table, so the value a run log records is the one that counted. */
static void warn_value(const char *name, const char *value, const char *levels)
{
    char line[256];
    snprintf(line, sizeof(line), "experiments: %s=\"%s\" is not one of %s; it is off\n", name, value, levels);
    fputs(line, stderr);
    size_t used = strlen(g_warnings);
    snprintf(g_warnings + used, sizeof(g_warnings) - used, "%s", line);
}

/* Unset, empty, "0" or "off": off without a warning. */
static bool off_value(const char *v) { return !v || !v[0] || !strcmp(v, "0") || !strcmp(v, "off"); }

/* SCRIBODO_OBSERVER_SENTENCES's levels (experiments.h): "attributes" (and "1", what every real run with "1" measured:
 * step 8), "subject"; off as off_value; anything else off with a warning. */
static ObserverSentencesArm observer_sentences_arm(void)
{
    const char *v = getenv("SCRIBODO_OBSERVER_SENTENCES");
    if (off_value(v)) return OBSERVER_SENTENCES_OFF;
    if (!strcmp(v, "attributes") || !strcmp(v, "1")) return OBSERVER_SENTENCES_ATTRIBUTES;
    if (!strcmp(v, "subject")) return OBSERVER_SENTENCES_SUBJECT;
    warn_value("SCRIBODO_OBSERVER_SENTENCES", v, "off, 0, attributes, 1 (= attributes), subject");
    return OBSERVER_SENTENCES_OFF;
}

/* SCRIBODO_OBSERVER_CONFLICT_GATE: "1" on; off as off_value; anything else off with a warning. */
static bool observer_conflict_gate(void)
{
    const char *v = getenv("SCRIBODO_OBSERVER_CONFLICT_GATE");
    if (off_value(v)) return false;
    if (!strcmp(v, "1")) return true;
    warn_value("SCRIBODO_OBSERVER_CONFLICT_GATE", v, "off, 0, 1");
    return false;
}

const char *observer_sentences_name(ObserverSentencesArm arm)
{
    return arm == OBSERVER_SENTENCES_ATTRIBUTES ? "attributes" : arm == OBSERVER_SENTENCES_SUBJECT ? "subject" : "off";
}

const char *experiments_warnings(void)
{
    experiments();
    return g_warnings;
}

static void read_environment(void)
{
    g_warnings[0] = '\0';
    Experiments e = {
        .answer_parts = flag_is("SCRIBODO_ANSWER_PARTS", "1"),
        .answer_block_in_message = flag_is("SCRIBODO_ANSWER_BLOCK", "message"),
        .memory_trace = flag_is("SCRIBODO_MEMORY_TRACE", "1"),
        .memory_excerpts = flag_is("SCRIBODO_MEMORY_EXCERPTS", "1"),
        .memory_small_units = flag_is("SCRIBODO_MEMORY_SMALL_UNITS", "1"),
        .semantic_debug = getenv("SCRIBODO_SEM_DEBUG") != NULL,
        .memory_experiment = flag_on("SCRIBODO_MEMORY_EXPERIMENT"),
        .memory_briefs = flag_is("SCRIBODO_MEMORY_BRIEFS", "1"),
        .answer_pipeline = flag_on("SCRIBODO_ANSWER_PIPELINE"),
        .answer_pipeline_requested = flag_is("SCRIBODO_ANSWER_PIPELINE", "verified"),
        .memory_presentation_compact = flag_is("SCRIBODO_MEMORY_PRESENTATION", "compact"),
        .memory_presentation_episode = flag_is("SCRIBODO_MEMORY_PRESENTATION", "episode"),
        .observer_conflict_gate = observer_conflict_gate(),
        .observer_sentences = observer_sentences_arm(),
        .fake_extract_ms = positive_int("SCRIBODO_FAKE_EXTRACT_MS"),
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
