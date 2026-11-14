/* The experiment flags, read from the environment once, in one place.
 *
 * Every flag here is an experiment with a dated record, and its default is the measured choice; the register entry
 * "experiment flags and columns" (docs/TECHNICAL-DEBT.md) is the list, and journal/answer-cycle4_flags.md the
 * measurement of this module. A flag that is not here does not exist: modules read experiments()->field, never
 * getenv. Flags that configure services rather than experiments (model paths, ports, secrets, the UI script host)
 * stay where they are read. Failed experiments stay in the tree flag-gated by the author's rule (MEMORY-015 §5:
 * delete nothing, only archive); this module is where they are gated.
 *
 * The table is read on first use and is immutable afterwards (one documented process-global, like the index-text
 * table); experiments_reload() re-reads it for tests that set the environment between calls, single-threaded. */
#ifndef SCRIBODO_EXPERIMENTS_H
#define SCRIBODO_EXPERIMENTS_H

#include <stdbool.h>

typedef struct {
    bool answer_parts;            /* SCRIBODO_ANSWER_PARTS=1: one candidate pool per question part (cycle 3 arm 4, pass 7: rejected) */
    bool answer_block_in_message; /* SCRIBODO_ANSWER_BLOCK=message: verified block in the newest message (pass 3a; 3b in the system message is the default) */
    bool memory_trace;            /* SCRIBODO_MEMORY_TRACE=1: MEMORY_PREPARE, ANSWER_PIPELINE and ANSWER_GATE lines on stderr */
    bool memory_excerpts;         /* SCRIBODO_MEMORY_EXCERPTS=1: the query-window excerpt variant (MEMORY-022: failed its gate) */
    bool memory_small_units;      /* SCRIBODO_MEMORY_SMALL_UNITS=1: 128-token document units, 16-unit packet (MEMORY-022: not promoted) */
    bool semantic_debug;          /* SCRIBODO_SEM_DEBUG set: failed Qdrant calls logged to stderr */
    bool memory_experiment;       /* SCRIBODO_MEMORY_EXPERIMENT=1: local memory (MEMORY-003 onward); every memory flag below needs it */
    bool memory_briefs;           /* SCRIBODO_MEMORY_BRIEFS=1: generated source-linked briefs, T1 (MEMORY-009: gated, default off) */
    bool answer_pipeline;         /* SCRIBODO_ANSWER_PIPELINE=verified: the verified-answer pipeline (journal/answer-cycle2_pipeline_live.md, pass 6) */
    bool memory_presentation_compact; /* SCRIBODO_MEMORY_PRESENTATION=compact: archived presentation (MEMORY-011: failed, kept for the harness) */
    bool memory_presentation_episode; /* SCRIBODO_MEMORY_PRESENTATION=episode: archived presentation (MEMORY-011: failed, kept for the harness) */
    bool observer_conflict_gate;  /* SCRIBODO_OBSERVER_CONFLICT_GATE=1: the canon candidate step ignores shared titles and the judge must name
                                     the two disagreeing values (journal/observer-cycle2_quality.md step 5: measured 2026-11-14, off-fault
                                     conflicts rose on both novels; kept gated, default off) */
} Experiments;

const Experiments *experiments(void);
void experiments_reload(void);

#endif
