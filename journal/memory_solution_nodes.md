# NODES — the grain of the problem

## Observations (what the evidence establishes)

**N1. The failure is semantic authority, not retrieval.**
Native runs admitted the correct original sources, matched IDs across arms, and
still produced wrong role/decision/scope answers. Exact evidence does not
guarantee faithful interpretation. (MEMORY-011, -012, -013.)

**N2. The model can state correct content and still fail the commitment.**
Correct natural-language recall coexists with wrong categorical labels; label
order flips answers without new evidence. Meaning is not conserved across output
forms. This is why prompt relabeling and status classification did not cure it.

**N3. Explicit Recall/Draft specialization produced gains but failed the screen.**
It improved accepted/unaccepted choice handling and some draft form compliance.
It still misread authority, protected ambiguity, partial evidence, and task
length. A role label is not a role enforcement mechanism. (MEMORY-013.)

**N4. The repository already contains an author-ratified semantic layer.**
`records.h`: canon entries are explicit author-approved assertions, versioned,
sourced, retractable; nothing becomes canon except through an explicit author
action. Findings can be committed to canon from the UI. Chat messages have a
"Canon" action (it opens a canon draft, still requiring the author to commit).
This is a proposal → ratification → canon pipeline.

**N5. Chat memory does not use canon.**
Chat retrieval stages original chat/document excerpts and optional generated T1
briefs. `semantic_canon_for()` (committed canon only) is called by the observer
for conflict checks, never by chat or the context planner. The reliable layer is
outside the conversation; the fallible generated layer is inside it.

**N6. Generated aids can poison.**
A stale or wrong T1 brief biased both ordinary and explicit recall toward its
content. Source membership and warning labels do not establish precedence. Any
generated text that re-enters retrieval becomes durable-looking authority.

**N7. The current product path is inverted.**
Author-controlled, versioned, sourced text is treated as a side record; model-
generated paraphrase is treated as the conversational memory. The evidence says
the first is trustworthy and the second is not.

## Tensions (the real design conflicts)

**N8. Automation versus authority.**
Automatic inference is the feature users imagine; explicit ratification is the
only mechanism that survived scrutiny. You cannot have both at full strength
with the tested model. The tension must be resolved by choosing which surface
gets authority, not by averaging them.

**N9. Prompt inclusion versus host display.**
Adding canon to the prompt may improve answers, but the model can still misread
it (N2). If the authoritative answer lives only in generated prose, canon in the
prompt does not solve the authority problem. If the authoritative answer is
host-rendered exact text, the model's reading matters much less. The dependency
is: host display first, prompt influence second.

**N10. Coverage versus honesty.**
Any retrieval system sees a subset. A confident local "no" is not a
manuscript-wide absence. Host code knows the queried scope and can state
coverage; the model cannot reliably infer it. Honest absence is a product
feature, not a failure.

**N11. Curation friction versus canon emptiness.**
If authors will not ratify commitments, canon stays empty and reliable recall
degrades to evidence search. This is a behavioral risk, not an architectural
one. It determines whether the solution is sufficient, not whether it is sound.

**N12. Canon is assertion, not proof.**
An author-approved line says what the author asserts, not what the manuscript
contains. Canon must keep its source references and must never replace the
original text for quotation, dispute, or version tasks. Assertion and evidence
are complementary layers, not substitutes.

## Dependency map

- N4 + N5 → the unused asset and the gap.
- N1 + N2 + N3 → why the model cannot be the authority (N9).
- N6 → why generated text must not re-enter eligibility (N9, N12).
- N8 → the product decision that cannot be dodged.
- N10 → a host responsibility that no model change can absorb.
- N11 → the main empirical risk of the emerging path.

## Emerging solution direction (not yet the answer)

Use the existing author-ratified canon as the authoritative semantic memory;
make a deterministic, host-rendered recall surface the place where continuity is
displayed; keep the model as a proposal/drafting surface whose output never
becomes canon or retrieval evidence without explicit author action. The model
may still be wrong, but its errors stop being authoritative.
