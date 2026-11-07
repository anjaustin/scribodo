# LMM SYNTHESIS — make the author's committed words the memory

**Status: architecture direction and experiment plan, not implementation approval,
not a quality-gate pass, not a default-on change.** Production routing, prompts,
model, consent, tiers and defaults remain as they are. The author's pre-existing
`assets/sample/sample.md` edit remains untouched.

## The clean cut

**Continuity should be a property of the author's ratified record, not a
semantic inference the model must reproduce.**

The model may propose. The model may draft. The model may converse. It must not
be the authoritative source of what the author decided. Reliable recall is
produced by deterministic host rendering of:

1. committed canon — the author's own approved assertions, versioned and sourced;
2. exact evidence — immutable original spans and their receipts.

The model sits beside that record, not on top of it.

## Why this is the solution the evidence points to

| Evidence | Consequence |
|---|---|
| Native runs admitted correct originals and still failed authority, ambiguity and scope answers | Retrieval was not the bottleneck; semantic transformation was |
| Correct prose coexisted with wrong labels; label order flipped answers | Re-asking the model in a new output form does not create reliable authority |
| Explicit Recall/Draft specialization improved some cases but failed the screen | Role labels do not enforce roles |
| Generated T1 briefs biased answers toward stale content | Generated text that re-enters memory becomes durable-looking authority |
| `records.h` already defines canon as explicit, author-approved, versioned, sourced and retractable | The reliable layer exists |
| `semantic_canon_for()` searches committed canon only, but only the observer calls it | Chat never uses the reliable layer |
| Chat messages already have "Canon" (opens a draft); findings have "Commit to canon" | The ratification surface exists |

The product inverted its own architecture: it treated the author-approved record
as a side feature and the generated paraphrase as the conversational memory.
Invert it back.

## The model, formally

```
E  immutable evidence spans (original, version, range, quote)
P  proposals (observer findings, chat turns, model drafts) — noncanonical
C  canon assertions (author-ratified, versioned, sourced, retractable)
R  deterministic recall views over C and E
G  generated text (chat prose, drafts, summaries)

observe:   E → P            provisional
ratify:    P × author → C   explicit author action only
revise:    C × author → C'  versioned; history retained
retrieve:  query × C × E → R   host-constructed, deterministic
generate:  query × R → G    model; never authoritative
```

**Invariants**

- **I1.** Canon changes only through author action. No model edge into canon.
- **I2.** Generated text never enters evidence or retrieval eligibility automatically.
- **I3.** Recall views are host-constructed from exact text and bound provenance.
- **I4.** Authoritative UI displays the recall view, never unmarked generated prose.
- **I5.** Generated aids are always marked noncanonical and excluded from evidence ranking.
- **I6.** Scope and consent apply identically to canon and evidence.
- **I7.** Canon keeps its sources; it never replaces originals for quotation,
  dispute, or version tasks.

The key property: **a model error can degrade a draft or an aside; it cannot
corrupt the record of what the author decided.** That is the property every
previous design lacked.

## Execution plan

### P0 — Deterministic recall view (no model, highest confidence)

Add a recall surface that answers by showing, not generating:

- the current version of matching committed canon entries, exact text;
- matching original excerpts with document/revision/range/quote receipts;
- an explicit coverage line: how many canon entries and excerpts were searched
  in this project/scope/version, and whether nothing matched.

Reuse `records_list(REC_CANON)`, `semantic_canon_for()` (committed only), and the
existing memory retrieval/admission path. No new store, no new tier, no model
call in the authoritative path. Scope and consent follow the existing memory
rules; canon is project data and must not silently widen access.

**Exit:** every displayed assertion is exact, sourced and inspectable; absence is
stated as bounded coverage, never as manuscript-wide proof.

### P1 — Canon-aware chat reference (experimental, separate preregistration)

Only after P0 works, add committed canon to the chat request as a distinct,
exact, labeled block:

```
AUTHOR-COMMITTED CANON (author-approved assertions; exact text; not a summary)
[c-… v3] …
```

Keep originals and their handles. Keep T1 off. Do not let canon replace
evidence. Then measure honestly:

- Does chat use canon correctly, or does it over-anchor and write about the
  block?
- Does it still invent authority when canon is silent?
- Does it confuse canon assertions with quoted manuscript text?
- Does it cite canon handles correctly?

This is a new contract, not a retune of the failed Recall/Draft fixture. It
needs its own frozen DEV registration and a fresh held-out gate before any
default.

### P2 — Separate authoritative citation from model commentary in the UI

When chat touches author state, show a host-rendered exact canon/evidence block
next to the prose. The author should never have to decide whether a fluent
sentence is a remembered decision or a guess. A simple visual distinction
between "your committed record says…" and "the assistant suggests…" is the
product-level enforcement of I4.

Do not auto-commit from this surface. Do not bulk-commit. Show the exact proposed
text and source before any author action.

### P3 — Keep capture cheap, but never automatic

The capture surfaces already exist:

- chat turn → "Canon" (opens an editable canon draft);
- observer finding → "Commit to canon";
- record panel → edit, revise, retract.

Keep them one action. Add an observer proposal only where it can show exact
supporting text. Never let the observer or chat write canon directly. The
author's click is the warrant; the system's job is to make the warrant informed.

### P4 — Demote generated continuity aids

T1 briefs remain default-off and noncanonical. They should not be retrieval
eligibility, should not outrank originals, and should not be presented as the
authoritative continuity layer. If they ever earn admission, it must be as
clearly labeled proposals after a separate non-poisoning test. T2 remains
deferred.

## Evaluation

Measure axes separately; no mean can hide an authority violation.

1. **Record correctness (deterministic):** every P0 display is exact and sourced;
   coverage is accurate.
2. **False-authority incidents (must be zero by construction in P0):** generated
   text presented as a decision that is not canon.
3. **Chat fidelity (P1):** role, decision/proposal/rejection, protected
   ambiguity, partial evidence, task form.
4. **Curation behavior:** whether authors add/revise canon in real sessions;
   if canon stays empty, the architecture is sound but the product premise fails.
5. **Rubber-stamping:** whether one-click commits import unreviewed model text.
6. **Scope/consent:** canon obeys the same boundaries as evidence; no silent
   cross-project or cross-conversation use.
7. **Cost:** recall latency and prompt tokens; canon must stay bounded.

A P0 pass is not automatic continuity. A P1 pass is not held-out acceptance.
Neither is default-on, T1/T2 qualification, or an SLA.

## What this does not solve

- Automatic inference of author intent.
- Model instruction-following in drafts (length, form, tone).
- Whole-manuscript retrieval or Qdrant selection quality.
- The neural/training-level cause of the model's semantic failures.
- The behavioral question of whether authors will curate.

Those remain open and must not be claimed.

## Decision requested, not assumed

This pass recommends a product direction: **reliable, inspectable continuity now;
automatic continuity only if a separately qualified capability earns it.** It
does not silently replace the model, change production routing, or declare the
MVP complete. The next step is authorization to build P0 as a deterministic
recall surface, with P1 gated behind its own preregistered experiment.

## Phase records

- [RAW](memory_solution_raw.md)
- [NODES](memory_solution_nodes.md)
- [REFLECT](memory_solution_reflect.md)
- [TURBOCHARGER](memory_solution_turbo.md)
- Prior evidence: [MEMORY-011](../docs/MEMORY-011-native-continuity-dev.md),
  [MEMORY-012](../docs/MEMORY-012-causal-continuity.md),
  [MEMORY-013](../docs/MEMORY-013-explicit-operations-dev.md)

Method: Lincoln Manifold Method v2. The previously pinned method revision remains
the repository reference; no method-source substitution is claimed.
