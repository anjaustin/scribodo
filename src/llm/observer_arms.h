/* The observer's arms and what was measured of each (AUDIT-004 F5).
 *
 * An arm is one combination of the observer's experiment flags (core/experiments.h: observer_conflict_gate and the
 * levels of observer_sentences). The author is shown a measured figure beside every possible conflict
 * (ui/findings_view.c); it is the active arm's, read from this table, so promoting a flag or re-measuring an arm
 * changes the claim in one place. An arm nobody measured says so rather than borrowing another arm's figure. Every
 * combination has a row (tests/test_observer_text.c, observer_arm_table). */
#ifndef SCRIBODO_OBSERVER_ARMS_H
#define SCRIBODO_OBSERVER_ARMS_H

#include <stdbool.h>
#include <stddef.h>

#include "core/experiments.h"

typedef struct {
    bool conflict_gate;
    ObserverSentencesArm sentences;
    const char *name;       /* for run logs */
    const char *disclosure; /* the sentence shown beside possible conflicts */
    const char *measured;   /* the figure's source: counts, novels and journal step, or "not measured" */
} ObserverArm;

/* The row for the given flags (never NULL). */
const ObserverArm *observer_arm(const Experiments *e);

/* Every row, for tests and tools. */
const ObserverArm *observer_arms(size_t *n);

#endif
