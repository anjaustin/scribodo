# Canon-first continuity: a clear engineering path, a conditional product path

**2026-11-07. Architecture direction from an LMM pass.**

**Status: documentation and recommendation only.** This document does not
authorize production changes, does not declare a quality-gate pass, and does not
change model, prompts, routing, tiers, consent, or defaults. The author's
pre-existing `assets/sample/sample.md` edit is untouched.

---

## 0. The short answer

**Do we have a clear path to a solution?**

- **Reliable, inspectable continuity:** yes. The authoritative mechanism already
  exists in the repository and is mechanically enforceable. The missing work is
  wiring and a display surface, not a research breakthrough.
- **Automatic, model-inferred continuity:** no. The tested local model failed
  authority, ambiguity, and scope tasks even with correct evidence. No prompt,
  tier, or output form has survived. That requirement needs a separately
  qualified capability.
- **Product-market fit for the narrow path:** unknown. The architecture is sound,
  but it depends on authors actually ratifying commitments. That is a testable
  behavioral bet, not a proven outcome.

The rest of this document separates those three claims carefully.

### Status vocabulary

| Label | Meaning |
|---|---|
| **Established** | Supported by code inspection or the frozen experiments cited here |
| **Direction** | Design hypothesis with a mechanistic rationale; not yet tested as a product |
| **Unvalidated** | Requires a new, preregistered experiment before any claim |
| **Not claimed** | Explicitly outside what this pass establishes |

---

## 1. The problem, stated precisely

Scribodo's promise is: *remember what we are doing without taking over what the
book means.* The implementation has interpreted "remember" as a retrieval and
generation problem: find relevant prior text, put it in the model's context, and
ask the model to answer or draft faithfully.

That interpretation failed at the last step. The failures were not retrieval
misses:

- native runs admitted the correct original sources, matched source IDs across
  arms, preserved current requests, and still produced wrong authority,
  ambiguity, and scope answers;
- correct natural-language statements coexisted with wrong categorical labels;
  reversing label order changed answers without new evidence;
- explicit Recall/Draft specialization improved some cases but failed the frozen
  screen;
- generated T1 briefs could bias answers toward stale content even when correct
  originals were present.

The common structure is a **transformation from evidence to meaning** that the
host cannot verify and the model does not reliably perform. Retrieval fidelity is
not semantic fidelity.

### 1.1 What the evidence establishes

| Finding | Source |
|---|---|
| Correct sources do not guarantee correct author-state answers | MEMORY-011, MEMORY-012 |
| The failure is not a storage or retrieval bug | MEMORY-012 causal contrasts |
| Output form changes reliability; labels are not direct readings of comprehension | MEMORY-012 |
| Explicit Recall/Draft specialization is not sufficient | MEMORY-013 |
| Generated derived aids can poison original recall | MEMORY-010, MEMORY-011 |
| The model/runtime is not uniquely at fault; official reference inference reproduced the failures | MEMORY-012 |

### 1.2 What the evidence does **not** establish

- That no model can do this. The tested configuration failed; a different model
  is a separate capability decision.
- That the neural or training-level cause is uniquely identified.
- That a host-rendered workflow will be pleasant, adopted, or sufficient.
- That the product should stop generating. It should stop *authorizing*
  generation.

---

## 2. The reframe: this is an authority problem

The decisive distinction is **verification asymmetry**:

**Host code can verify**
- bytes, hashes, identities, versions, ranges;
- speaker and order metadata;
- scope, consent, provider, local-only status;
- token counts, resource bounds, admission;
- exact quotation and handle binding;
- whether an author action occurred.

**Host code cannot verify**
- whether a paraphrase preserved a commitment;
- whether "maybe" was a decision;
- whether an omission proves absence;
- whether the model attributed a choice to the right speaker;
- whether a generated sentence entails its cited source.

Every failed design routed a semantic commitment through generated text, creating
an unverifiable dependency. The fix is not to make the transformation stronger.
It is to remove it from the authoritative path.

> **Core insight:** Reliable continuity requires that the author's own committed
> words — not the model's inferences — be the authoritative memory, and that this
> memory be rendered deterministically rather than regenerated.

This is the same note that kept reappearing as a "narrower author-controlled
workflow." It was not a fallback. It was the shape the evidence pointed at.

---

## 3. The reliable layer already exists — and is unused

### 3.1 Canon is author-ratified, versioned, sourced

`src/app/records.h` defines the contract:

- notes are editable, non-authoritative records;
- **canon entries are explicit, author-approved assertions**;
- status is `committed` or `retracted`;
- **nothing becomes canon except through `records_append()` called for an explicit
  author action**;
- the log is append-only; every earlier version stays readable;
- each source reference names a document revision, range, and quote, or a chat
  message and range, so provenance survives later edits.

This is not an aspiration in a design doc. It is the existing storage and UI
contract.

### 3.2 The proposal → ratification pipeline exists

- Chat messages expose a **Canon** action. It opens a canon *draft* prefilled
  with the message and a chat source; the author must still commit it
  (`src/ui/chat_view.c`, `src/main.c:record_from_reply`).
- Observer findings expose **Discuss**, **Save as note**, **Commit to canon**,
  **Dismiss**, and **Source** (`src/ui/findings_view.c`). The observer itself never
  writes canon.
- The record panel supports note → canon, canon draft → commit, and committed
  canon → revise/retract (`src/ui/record_view.h`).

So the system already has the right *shape*: model/observer proposes; author
ratifies; canon records the result with sources.

### 3.3 Canon is searchable — but chat does not use it

`semantic_canon_for()` (`src/llm/semantic.c`) searches only `kind=canon` with
`status=committed` and explicitly excludes retracted entries, notes, draft text,
and chat. It is used by the observer for conflict checks
(`src/llm/observer.c`).

**It is not called by chat or the context planner.** Chat memory
(`src/llm/context_planner.c`) stages original excerpts and optional generated T1
briefs; its own header says the material is "reference data, not instructions or
canon." The memory notice says "Sources are as-of this reply, not canon."

The product built the trustworthy semantic layer and then built a fallible
generative layer beside it, and asked the fallible one to carry the conversation.

### 3.4 There is no dedicated recall surface

The current UI has chat, notes, canon records, findings, sources, explorer, and
editor panels. There is no deterministic "show me what my record says about X"
recall view. Recall is currently expected to happen inside generated chat.

---

## 4. Target architecture

### 4.1 Entities

```
E  immutable evidence spans (original, document/revision/range/quote,
   or chat id/message/range)
P  proposals (observer findings, chat turns, model drafts) — noncanonical
C  canon assertions (author-ratified, versioned, sourced, retractable)
R  deterministic recall views over C and E
G  generated text (chat prose, drafts, summaries)
```

### 4.2 Transitions

```
observe:   E → P              provisional; model or observer
ratify:    P × author → C     explicit author action only
revise:    C × author → C'    versioned; history retained
retract:   C × author → C⁻    committed → retracted; history retained
retrieve:  query × C × E → R  host-constructed, deterministic ranking
generate:  query × R → G      model; never authoritative
```

### 4.3 Invariants

- **I1.** Canon changes only through author action. There is no model edge into
  canon.
- **I2.** Generated text never enters evidence, canon, or retrieval eligibility
  automatically.
- **I3.** Recall views are host-constructed from exact text and bound provenance.
- **I4.** Authoritative UI displays the recall view, never unmarked generated
  prose.
- **I5.** Generated aids are always labeled noncanonical and excluded from
  evidence ranking and eligibility.
- **I6.** Scope and consent apply identically to canon and evidence. Canon does
  not silently widen access.
- **I7.** Canon keeps its sources; it never replaces originals for quotation,
  dispute, or version tasks.

### 4.4 Failure containment

The property this architecture buys:

> **A model error can degrade a draft or a conversational aside, but it cannot
> corrupt the record of what the author decided.**

Previous designs failed because generated text sat on the path from evidence to
the author's answer. The new design keeps generated text off that path.

### 4.5 Why canon-first reduces the model's hardest job

The failing cases were dominated by **role and authority inference**: assistant
suggestion versus author decision, pending versus protected ambiguity, local
evidence versus manuscript-wide absence. Canon supplies the author's assertion
explicitly, so the model no longer has to infer it. The model still has to read
and use canon, but the authoritative display does not depend on that reading.

This is a mechanistic link between the diagnosis and the remedy, not just a
convenient reuse of existing code.

---

## 5. The clear engineering path

The path is **clear for P0**, conditional for P1, and intentionally deferred for
anything that depends on model semantic authority.

### P0 — Deterministic recall view (no model)

**Goal:** make authoritative recall a host-rendered, inspectable surface.

**Behavior**

- Accept a query and a project/scope.
- Retrieve:
  - matching committed canon entries (current version, exact text);
  - matching original excerpts with document/revision/range/quote or chat
    message/range receipts.
- Render, for every result:
  - the exact text;
  - its kind (author-committed assertion vs original evidence);
  - its source and version;
  - when it was recorded / the revision it belongs to.
- Render an explicit coverage line:
  - how many canon entries and evidence candidates were searched;
  - which project/scope/version was searched;
  - whether nothing matched.
- Never generate prose in this surface.

**Reuse**

- `records_list(REC_CANON)` / `records_list_checked` for current canon versions.
- `semantic_canon_for()` for committed-canon ranking (committed only).
- The existing retrieval/admission path for original excerpts, or a read-only
  subset of it.
- Existing `MemoryScope` / consent / local-only logic.

**Do not**

- auto-write canon;
- include notes or findings as authoritative;
- claim manuscript-wide absence;
- widen project/conversation access;
- use generated T1 briefs.

**Acceptance**

- Deterministic tests prove every displayed canon line equals the current
  committed version text and status.
- Every source quote resolves at the recorded revision/range, or the result is
  rejected.
- Coverage counts are accurate for the searched scope.
- Cross-project, revoked-consent, remote-provider, and stale-identity negative
  tests fail closed.
- No model call occurs on the authoritative path.

**Why this is the highest-confidence step:** it is deterministic. It does not
depend on model behavior, prompt wording, or semantic inference.

### P1 — Canon-aware chat reference (experimental, separately gated)

**Goal:** test whether exact author-committed canon improves conversational
continuity without creating a new authority trap.

**Behavior**

- Add a distinct, exact block to the chat request:
  `AUTHOR-COMMITTED CANON (author-approved assertions; exact text; not a summary)`.
- Include version/status and short handles.
- Keep original evidence and handles.
- Keep T1 off.
- Keep canon out of evidence ranking; it is a separate reference class.
- Do not let canon replace originals for quotation or version tasks.

**Measure**

- correct use of canon;
- over-anchoring / writing about the canon block;
- invented authority when canon is silent;
- confusion between canon assertions and quoted manuscript text;
- citation/handle correctness;
- task form and completion.

**Gate**

This is a **new contract**, not a retune of the failed Recall/Draft fixture. It
needs its own frozen DEV registration, review rubric, and stop condition. A pass
is still not fresh held-out acceptance, default-on, or T1/T2 qualification.

### P2 — UI separation of authority and commentary

**Goal:** make it impossible for a fluent sentence to masquerade as a remembered
decision.

**Behavior**

- When chat touches author state, show a host-rendered exact canon/evidence block
  beside the prose.
- Visually distinguish “your committed record says…” from “the assistant
  suggests…”.
- Link every authoritative claim to its record/source.
- Do not auto-commit from this surface.
- Do not bulk-commit.

This is the product-level enforcement of I4. P0 proves the record can be shown;
P2 makes the distinction visible where authors actually read.

### P3 — Cheap capture, never automatic

**Goal:** keep ratification cheap enough to be used, without letting the model
write canon.

**Existing surfaces**

- chat → “Canon” draft (editable, still requires commit);
- finding → “Commit to canon”;
- record panel → commit, revise, retract.

**Rules**

- show the exact proposed text and source before commit;
- never pre-commit, auto-commit, or bulk-apply;
- let the author edit before committing;
- keep the action one step, not a workflow.

**Open product question:** whether the observer should propose decision-like
entries. It may, but only as clearly labeled proposals with exact support. Its
proposal quality is not an authority mechanism.

### P4 — Demote generated continuity aids

- T1 briefs remain default-off and noncanonical.
- They are not eligibility, do not outrank originals, and are not the
  authoritative continuity layer.
- If they ever earn admission, it must be after a separate non-poisoning test and
  they must remain labeled proposals.
- T2 remains deferred.

### Sequencing

```
P0 (deterministic, no model)
   ↓
P3 capture usability (can run in parallel; no model authority)
   ↓
P1 canon-aware chat (experimental; separate registration)
   ↓
P2 UI authority separation (can start with P1; requires P0)
   ↓
P4 demotion is a standing policy, not a one-time migration
```

P0 and P3 do not depend on any model experiment. P1/P2 do.

---

## 6. What is a bet, not a path

### B1. Authors will curate canon

This is the load-bearing behavioral assumption. If canon stays empty, the
architecture degrades to evidence search. That may still be useful, but it is not
continuity.

The existing one-click capture surfaces reduce friction. Whether that is enough
is an empirical product question. It must be measured, not assumed.

### B2. Canon-aware chat helps rather than over-anchors

The earlier oracle-card experiment showed that hand-written continuity cards can
induce card-writing or unsupported detail. Exact author text is not automatically
safe in a prompt. P1 must test for over-anchoring, not just accuracy.

### B3. Inspectable continuity is what authors want

The architecture trades magic for auditability. The evidence favors it for
high-stakes canon, but it is a product preference, not a proven one. Some authors
may prefer a confident assistant even when it is occasionally wrong. The honest
response is to measure trust and correction behavior, not to assert preference.

### B4. Scope and consent remain coherent

Canon is project-level. Memory scope is project/conversation with consent and
local-only rules. Whether canon should be available in every conversation in a
project, or only where the author opted in, is a design decision. It must be
explicit. Canon must not become a backdoor around memory scope.

### B5. A future model may make automatic continuity viable

If a separately qualified model reliably preserves authority on held-out cases,
the narrow path can relax. It should still keep auditability for canon, because
authors need to know why a decision is recorded. A better model writes better
proposals; it does not remove the need for ratification at the point of
commitment.

---

## 7. The fork

### Definition A — reliable, author-controlled continuity

**Question:** Can the author inspect, trust, and correct what the system remembers?

**Answer:** Yes, this is achievable with existing assets. P0 is deterministic.
The model is optional for the authoritative surface.

**What it guarantees:** exact display, provenance, versioning, explicit
authority, bounded coverage, no silent model canon.

**What it does not guarantee:** automatic capture, conversational fluency about
author state, drafting quality, whole-manuscript recall.

### Definition B — automatic, model-inferred continuity

**Question:** Can the assistant remember what the author decided without the
author curating it?

**Answer:** No clear path with the tested configuration. The failures are not
retrieval bugs and did not yield to prompt specialization. A different capability
is required, separately authorized and measured.

**What it would require:** a model that passes fresh, held-out authority,
ambiguity, scope, and citation gates; a non-poisoning derived-state design; and
a consent/scope model for inferred state.

These are different products. Conflating them is what produced four cycles of
failed experiments. The decision is not “which prompt” but “which promise.”

---

## 8. Evaluation plan

No aggregate mean may hide an authority violation. Evaluate axes separately.

### 8.1 Deterministic record gates (P0)

| Axis | Test | Pass condition |
|---|---|---|
| Exactness | Displayed canon text vs current committed version | Byte-exact |
| Provenance | Source quote resolves at document revision/range or chat message/range | Resolves or result rejected |
| Status | Only `committed` canon is authoritative | Retracted/notes/findings excluded |
| Coverage | Search scope and counts | Accurate; absence stated as bounded |
| Scope | Cross-project, revoked consent, remote, stale identity | Fail closed |
| Cost | Latency and rendered bytes | Within existing bounds |
| No-model | Authoritative path makes no inference call | Verified by construction/test |

These can be validated without a model and without a human quality score.

### 8.2 Curation gate (product behavior)

Instrument real or representative sessions:

- how often authors create/revise canon;
- how often they commit a proposal unchanged (rubber-stamp proxy);
- how often they edit before committing;
- whether decision-bearing sessions end with an empty record.

**Pre-register the threshold before the study.** Do not retrofit a threshold after
seeing the data. A candidate experimental hypothesis — not a production SLA — is
that a majority of decision-bearing sessions should produce at least one canon
action. If curation is near zero, P0 is insufficient and the narrow product
premise is not validated.

### 8.3 Chat fidelity gates (P1)

Same axes as the prior native work, now with canon present:

- role and authority;
- accepted / proposed / rejected choices;
- pending versus protected ambiguity;
- partial evidence and bounded absence;
- exact current-task execution;
- citation and handle correctness;
- completion and form;
- over-anchoring on the canon block;
- harmful influence when canon is stale or wrong.

### 8.4 Held-out acceptance

Any claim of general continuity still requires fresh, unseen cases, frozen
protocol, frozen model/config/budgets, and a conjunctive decision. The failed V2,
native, causal, and operation fixtures remain negative/diagnostic evidence; they
are not fresh wins.

### 8.5 False-authority metric

Define it explicitly:

> A **false-authority incident** is any generated statement presented as an
> author decision, constraint, or protected state that is not backed by a current
> committed canon entry or exact author evidence.

Target for the authoritative surface: **zero by construction**. Target for chat:
measured and disclosed; not assumed to be zero. A model error that is clearly
marked as commentary is a quality issue; an error presented as the record is an
authority failure.

---

## 9. Risks and mitigations

| Risk | Why it matters | Mitigation |
|---|---|---|
| Empty canon / curation friction | Architecture degrades to search | One-click capture; observer proposals with exact support; measure curation; do not pretend automation exists |
| Rubber-stamping | Author approval can launder model text | Show exact text + source before commit; no pre-commit or bulk commit; measure edit-before-commit rate |
| Stale canon | Author changes mind without revising | Show timestamps/versions; current request overrides; prompt to revise; stale-but-auditable is safer than silent hallucination |
| Contradictory canon | Old and new assertions conflict | Versioning + retraction; observer conflict findings; never auto-resolve |
| Scope/consent leak | Canon is project-level; memory is scoped | Explicit policy; apply the same consent/eligibility rules; negative tests |
| Author confusion: assertion vs evidence | Canon says what the author asserts, not what the text contains | Show both layers; link sources; use originals for quotation/version tasks |
| Over-anchoring in chat | Prompt canon can dominate the task | P1 tests; keep canon a separate reference class; keep originals |
| “This is just search” | Product may feel like a regression | Emphasize inspectability and exactness; integrate with chat, not replace it; measure trust/time-to-verify |
| Canon growth | Long-term retrieval and cost | Bounded retrieval; current-version only; archive/retract; existing budgets |
| Multi-project/version | Wrong canon could be shown | Filter by project; bind revisions; fail closed |

---

## 10. What this does not solve

- Automatic inference of author intent.
- Model instruction-following in drafts (length, form, tone).
- Whole-manuscript retrieval quality or Qdrant selection quality.
- The neural/training-level cause of the model's semantic failures.
- T1 redesign or qualification; T2; default-on; SLAs.
- Whether authors will curate or prefer inspectable continuity.
- Whether a larger/different model would make the automatic path viable.
- Cryptographic or semantic proof that a canon assertion is true; canon records
  author authority, not factual truth.

---

## 11. Non-goals and guardrails

**Non-goals for this direction**

- No new memory tier.
- No automatic intent classifier/router.
- No model-written canon.
- No replacement of originals with summaries.
- No silent expansion of memory scope.
- No default-on change.
- No model swap on the strength of this document.

**Guardrails**

- Local-first; consent and scope apply to canon as to evidence.
- Immutable provenance for every authoritative display.
- Bounded candidates, bytes, and tokens.
- Fail closed on stale identity, revoked consent, remote selection, or missing
  provenance.
- Preserve negative results; do not retune failed fixtures until green.
- Any new operation contract or capability experiment is separately identified
  and preregistered.

---

## 12. Decision record

**Decision requested:** authorize P0 as a deterministic recall surface, with P1
behind its own frozen experiment and fresh held-out acceptance.

**What is authorized by this document:** nothing operational. It is a direction
and a plan.

**What is explicitly not authorized:** production routing changes, model swap,
prompt changes, tier changes, consent/default changes, automatic canon writes, or
any claim that the MVP is complete.

**What would change the decision:**

- P0 deterministic tests fail → fix or stop; do not paper over with generation.
- Curation study shows near-zero author use → the narrow product premise is not
  validated; revisit the product bet.
- P1 fails → keep P0 authoritative and the model non-authoritative; do not
  retune the same fixture.
- A separately qualified model passes held-out authority gates → consider
  relaxing the narrow path, but keep auditability for commitments.

---

## 13. Open questions

1. Will authors curate canon in real sessions, and at what friction?
2. Should canon be project-wide or conversation-scoped for chat reference?
3. How should absence be phrased so it is honest without being useless?
4. Should protected ambiguity be a first-class canon type, or free-form text?
5. How should contradictory canon entries be surfaced and resolved?
6. Can the observer propose decision-like entries without creating noise?
7. Does exact canon in the prompt help, over-anchor, or both?
8. How do we prevent rubber-stamping without making ratification heavy?
9. What is the minimum recall UI that authors will actually use?
10. Is inspectable continuity sufficient for the product promise, or is automatic
    capture non-negotiable?

These are deliberately left open. None is a hidden commitment.

---

## 14. Relationship to prior work

| Document | Contribution to this direction |
|---|---|
| MEMORY-009 | Resource and brief experiment; T1 limits |
| MEMORY-010 | Bedrock diagnostics: composition sensitivity, citation ambiguity, poisoning |
| MEMORY-011 | Native continuity no-go; admission repair; context costs |
| MEMORY-012 | Causal investigation: authority meaning is not conserved across forms |
| MEMORY-013 | Explicit Recall/Draft specialization: mixed gains, screen failed |
| MEMORY-014 (this) | Reframe to authority; canon-first architecture; P0–P4 path |

The prior negative results remain binding. This document does not overturn them;
it changes where the product places trust.

---

## 15. Phase records

- [Synthesis](../journal/memory_solution_synth.md)
- [RAW](../journal/memory_solution_raw.md)
- [NODES](../journal/memory_solution_nodes.md)
- [REFLECT](../journal/memory_solution_reflect.md)
- [TURBOCHARGER](../journal/memory_solution_turbo.md)

**Code references**

- `src/app/records.h` — canon contract, versioning, source refs.
- `src/llm/semantic.c` — `semantic_canon_for()` (committed canon only).
- `src/llm/observer.c` — canon used for conflict checks; observer never writes canon.
- `src/llm/context_planner.c` — original excerpts + generated T1 briefs; chat memory.
- `src/llm/chat.c` — memory plan apply/admission; memory notice says sources are not canon.
- `src/ui/chat_view.c`, `src/main.c` — chat “Canon” draft action.
- `src/ui/findings_view.c` — “Commit to canon” and related finding actions.
- `src/ui/record_view.h` — note/canon panel actions and version history.

**Method:** Lincoln Manifold Method v2. The previously pinned method revision
remains the repository reference; no method-source substitution is claimed.
