# First diagnostic result

Core finding: a correct source in a rendered request is insufficient evidence
that the composed multi-turn request behaves like a single-fact lookup.

With the pinned production model and flags, the minimal lookup returns 47. A
single framed/strict-JSON request also returns 47, though one citation ID is an
entire header rather than its unit ID. Adding one irrelevant prior user turn
still works. Two prior user turns cause abstention. At 64 prior user turns the
model describes irrelevant weather history. Rendering retains the fact/question
in all cases. Combining the 64 history paragraphs into one user turn restores
the factual answer, but not a valid original ID. A later repeated minimal
lookup again returns 47.

This breaks the broad hypothesis that the checkpoint cannot perform the basic
lookup, and distinguishes factual comprehension from citation-schema fidelity.
It does not yet distinguish trained role sensitivity from an implementation
problem. The two-turn failure also crosses 256 prompt tokens, so batch/sliding-
window interactions require a control before assigning causation to roles.

Next controls: same cases at batches 64/256/512; inherited versus disabled
repetition penalty; unique versus repeated prior turns; short-system/plain
answers versus long strict-JSON instructions; combined two-turn text. These are
diagnostic ablations, not modifications to frozen evaluation or product defaults.
