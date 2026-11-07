# TURBOCHARGER — testing the edge

Claim under test: *Use author-ratified canon as the authoritative semantic
memory, render it deterministically, and keep the model as a non-authoritative
proposal/drafting surface.*

## TEST 1 — Steel-man the opposite

**Strongest case against:** This is manual knowledge management dressed up as
architecture. The entire value of AI memory is that the author does not have to
curate. Authors will not maintain canon; the observer will miss the decisions
that matter; canon will be empty or stale; and chat will keep answering from raw
excerpts anyway. Meanwhile a deterministic recall panel is just search, which
the product already has. The real fix is a stronger model, not a new filing
chore. You are optimizing for auditability when users want magic.

**What survives:** The critique lands on *sufficiency*, not *soundness*. It is
true that canon-first does not create continuity by itself; it depends on
capture. But the alternative on the table is not magic — it is a model that was
measured failing at exactly this task, repeatedly, with correct evidence. A
confident wrong memory is worse than an inspectable one. The counterargument
also assumes curation must be heavy. The repository already has one-click
"Canon" on chat messages (a draft the author still commits) and "Commit to
canon" on findings. The design
question is whether that is cheap enough, which is testable.

**Damage:** The claim "this solves continuity" is too strong. It becomes: *this
is the only reliable continuity mechanism available with the tested capability,
provided capture is cheap enough that authors actually use it.*

## TEST 2 — Find the historical failure

- **ChatGPT memory:** stored inferred facts users could not easily audit; wrong
  memories persisted and shaped answers. The fix was visibility and editing,
  not better extraction alone.
- **Microsoft Recall:** captured everything by default; the failure was consent
  and scope, not retrieval quality. Scribodo's local-first, opt-in posture is
  the right side of that line, but canon inclusion must still respect scope.
- **Enterprise knowledge graphs / semantic layers:** extraction errors and
  schema drift turned "knowledge" into maintenance debt. Inferred state rots;
  authored state can be revised.
- **Clippy / proactive agents:** unwanted agency erodes trust. Model proposals
  must never silently become commitments.
- **Git:** the closest positive precedent. Commits are explicit, authored,
  versioned, and auditable; the working tree is provisional. People tolerate
  commit friction because the history is worth it. Canon is a commit; chat is a
  working tree.

**What survives:** Every failure mode is about *unreviewed inferred state
becoming authoritative*. Canon-first is the architectural answer to that class
of failure. The Git analogy also exposes the cost: commits need a diff and a
reason. Canon needs a visible source and a short assertion, not a heavyweight
workflow.

## TEST 3 — Reverse the assumptions

| Assumption | Inversion | Danger |
|---|---|---|
| Model must interpret author state | Author states it; model only proposes | Curation burden, empty canon |
| Correct context yields correct answer | Correct context plus model authority still fails | Prompt-only canon is insufficient |
| Canon can stand in for evidence | Canon is assertion; evidence is proof | Misquoting or hiding version changes |
| Memory must be generated | Memory can be retrieved and displayed | Product may feel less "AI" |
| One project-wide memory is fine | Scope and consent are per project/conversation | Silent access widening |
| Ratification guarantees truth | Ratification can be rubber-stamped | Author-approved model text can still be wrong |

The most dangerous inversion is the last: **explicit approval is not the same as
careful approval.** One-click commit can launder a model suggestion into canon.
This does not invalidate the architecture, but it means the UI must show the
exact proposed text and its source, and the system must never pre-commit or
bulk-commit. The author's action is the warrant; the system's job is to make the
warrant informed.

## TEST 4 — Who disagrees, and are they right?

- **Scaling optimists:** a larger model will preserve intent reliably. Possibly.
  But the architecture is model-agnostic: a better model writes better proposals
  and drafts; authority still needs audit. Under the current local envelope, the
  claim is untested at scale.
- **UX researchers:** authors will not curate. This is the strongest objection.
  It is an empirical product risk, not a logical refutation. It must be measured,
  not assumed away.
- **Product marketers:** deterministic recall does not demo like generation.
  True, and irrelevant to correctness. Trust is the feature.
- **Privacy advocates:** local canon with source refs is safer than cloud memory.
  Agree; scope/consent must extend to canon explicitly.

The disagreement that matters is the UX one. The architecture is sound; its
adoption is not proven.

## TEST 5 — Scale attack

At 300,000 words and years of use, canon grows. Three pressures appear:
1. **Retrieval:** bounded ranking over canon, not unbounded dump. Existing
   candidate/reference budgets must apply.
2. **Contradiction:** old and new canon entries conflict. The observer already
   detects conflicts against current canon; canon versioning and retraction
   handle the rest. Conflicts must be surfaced, never auto-resolved.
3. **Staleness:** the author changes their mind in chat but does not revise
   canon. The system will show stale-but-auditable state. That is a safe
   failure, unlike a silent hallucination, but it must be visible (timestamps,
   versions, "current request overrides").

At 0.1x (a short session), canon-first is overkill but harmless; evidence search
alone suffices. The architecture degrades gracefully.

## TEST 6 — Formalization

If the insight cannot be stated as invariants, it is vague. Here is the model.

**Entities**
- `E`: immutable evidence spans (originals, versions, ranges, quotes).
- `P`: proposals (observer findings, chat messages, model drafts). Non-canonical.
- `C`: canon assertions (author-ratified, versioned, sourced, retractable).
- `R`: deterministic recall views over `C` and `E`.
- `G`: generated text (chat prose, drafts, summaries).

**Transitions**
- `observe: E → P` (model/observer; always provisional)
- `ratify: P × author → C` (explicit author action only)
- `revise/retract: C × author → C'` (versioned; history retained)
- `retrieve: query × C × E → R` (host-constructed, deterministic ranking)
- `generate: query × R → G` (model; never authoritative)

**Invariants**
- **I1.** `C` changes only through author action. No model edge into `C`.
- **I2.** `G` never enters `E`, `C`, or retrieval eligibility automatically.
- **I3.** `R` is host-constructed from exact text and bound provenance.
- **I4.** An authoritative UI displays `R`, never unmarked `G`.
- **I5.** Generated aids are `G`; if shown, they are labeled noncanonical and
  excluded from eligibility and ranking as evidence.
- **I6.** Scope and consent apply identically to `C` and `E`.
- **I7.** Every `C` keeps its source references; canon never replaces evidence
  for quotation, dispute, or version tasks.

**Failure containment.** A model error in `G` can degrade a draft or a
conversational aside. It cannot corrupt `C` or the authoritative `R`. This is
the property the previous designs lacked: they put `G` on the path from `E` to
the author's answer.

**What the model exposes:** the initial claim "add canon to the chat prompt" is
*not* sufficient under these invariants, because prompt-only canon leaves the
authoritative answer inside `G`. The model forces the host-rendered `R` to be
primary. That is a genuine refinement, not a restatement.

## TEST 7 — Does ratification actually happen mechanically today?

Yes for canon: `records.h` says nothing becomes canon except through an explicit
author action, and `records_append` is called from UI actions. The observer
explicitly never writes canon. Chat has a "Canon" action that opens a draft; findings have
"Commit to canon." The invariant is enforceable in existing code, not merely
aspirational.

But the *retrieval* wiring is missing: `semantic_canon_for()` is committed-canon
only and is called by the observer, not by chat or the planner. The solution is
therefore not a new subsystem; it is a reconnection of an existing authoritative
layer to the surfaces that need it.

## TEST 8 — The strongest remaining attack: "The model is just too weak; fix the model."

If a future model reliably preserves authority on held-out cases, canon-first is
still defensible for auditability, but its necessity weakens. The honest claim
is conditional:

> Under the tested local capability envelope, and for commitments whose errors
> are costly and hard to detect, semantic authority must not flow through
> generated text.

A better model can be adopted through the same proposal/ratification gate. It
does not require inverting the architecture. Crucially, the architecture also
gives a future model a cleaner job: propose and draft, not adjudicate truth.

## What broke

1. **"Canon in the prompt is the solution."** Broken. Prompt-only canon recreates
   the interpretation dependency. Host-rendered recall must be primary.
2. **"Canon-first is automatic."** Broken. Capture is load-bearing; empty canon
   means evidence search, not continuity.
3. **"Canon replaces evidence."** Broken. Assertion and evidence are distinct;
   quotation and version tasks need originals.
4. **"One-click commit is safe."** Partly broken. Rubber-stamping can launder
   model text; commit UI must show exact text and source, and never bulk-apply.
5. **"This fixes chat fidelity."** Broken. Chat can still misread canon; the fix
   is UI separation and host-injected exact citations, plus measurement.

## Refinement (complexity removed)

The final model is smaller than where we started: no new semantic ledger, no new
extraction model, no new memory tier, no automatic router, no change to the
local model. Reuse `records` canon, `semantic_canon_for`, exact evidence, and
the existing receipt/consent machinery. Add a deterministic recall view and
canon-aware retrieval. Demote generated T1 aids from the authoritative path.
The only genuinely new surface is the host-rendered recall view.

## Convergence check

After refinement, the remaining objections are behavioral (will authors curate?)
and UI-level (will users trust chat prose over the panel?). Both are measurable.
No new architectural failure mode appeared, and the design got simpler. Stop
here; further prompt/model experiments would be premature until the
authority-preserving path is tested.
