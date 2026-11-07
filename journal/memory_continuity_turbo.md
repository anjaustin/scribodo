# LMM TURBOCHARGER — TEST / MODEL / REFINE

These cycles preserve observations, counterexamples and decision rationale, not
private deliberation. Distinguish empirical checks, code inspection and design
properties; none is a substitute for held-out native acceptance.

## Cycle 1 — attack the proposed cure

### TEST: strongest alternatives and reversed assumptions

**Opposite position:** keep only a native recent tail plus raw retrieval. Typed
state, generated briefs and a semantic ledger may cost more, introduce false
beliefs and make the request harder to follow. They must beat that baseline,
not merely look organized.

Sixteen new diagnostic generations used the pinned local runtime and production
persona, with four scenarios and four representations:

- unaccepted assistant proposal;
- author-rejected plan;
- next action / pending choice / persistent constraint;
- deliberately unresolved fictional cause.

Representations: native dialogue, quoted originals, hand-written correct state
card plus originals (**oracle control, not an implemented extractor**), and a
deliberately lossy/stale card labelled noncanonical plus originals. Expected
behavior is offline metadata, not injected into the current question. The
oracle card intentionally supplies a correct interpretation to test an upper
bound; it cannot establish extraction quality or a winning production design.

Observed qualitative results:

- The native and quoted cases usually recognize the unaccepted proposal and
  rejected ferry plan. Basic decision distinctions are not universally beyond
  this checkpoint.
- A correct oracle card is not uniformly better. It sometimes triggers a new
  working-card/scene-outline response instead of a concise recall response.
- The resume task exposes a semantic distinction between an undecided narrator
  and a bell that must remain ambiguous; a generic list of “open questions” can
  collapse the two.
- In the deliberate-ambiguity case, a lossy card biases the output toward “the
  fire was caused by someone pushing the lamp,” despite the original instruction
  to keep both accounts plausible. The output then hedges; it is not a clean
  preservation of the author's deliberately unresolved state.
- Eight outputs hit the shared 128-token allowance. Partial responses must not
  be reported as complete successes. These are qualitative diagnostics, not a
  scored quality comparison; prompt lengths differ and extra card text is charged.

**Claim damaged:** “a correct, source-linked state card is the solution.” The
model can have the right information and still perform the wrong task, or let
a stale derived aid distort the interpretation of correct originals.

### MODEL: separate the contracts

```text
authorized original events + current request
                |
        bounded context assembly
        - preserve speaker and order
        - preserve proposal/reply adjacency
        - keep original-source handles
                |
       model performs the present task
       recall / compare / brainstorm / draft
                |
      answer + host-owned source receipt
```

Not every unknown is a TODO:

```text
assistant proposal -> author accepts / rejects / leaves optional
pending choice    -> may need a future decision
deliberate ambiguity -> must remain unresolved unless author changes direction
```

These are distinctions the response must respect, **not** a claim that a model
can reliably populate a new authoritative state machine. Preserve original
events; do not overwrite them with an inferred terminal state.

### REFINE: remove premature machinery

Drop a semantic ledger and automatic mode classifier from the first proposed
slice. Test simpler original-source packets and explicit current-task behavior.
Keep generated aids optional and replaceable, not a second truth authority.

## Cycle 2 — test the simpler model against its boundaries

### TEST: concrete counterexamples

1. **Adjacency loss:** assistant proposes two endings; author says “yes, but not
   the second one.” Sorting all author lines separately from assistant lines
   destroys the link. Group only with exact order/roles and bounded neighbors.
2. **Branch/retcon:** a newer message quotes an old draft. “Newest wins” would
   restore a discarded choice. Compare explicit versions or ask; sequence is
   not story truth or universal supersession.
3. **Attribution:** a narrator says the lamp fell; a character says it was pushed.
   A quote can be exact and still not entail the answer. Host source membership
   and quote checks cannot replace semantic/authority tests.
4. **Intent override:** a current author request intentionally changes an earlier
   style rule. Historical constraints cannot outrank the current request or
   become system instructions. Recalled text never gains mutation/provider access.
5. **Salience reversal:** scenery dismissed as noise later becomes the important
   clue. Keep all originals; no universal salience filter or source deletion.
6. **Sparse evidence:** one selected passage cannot prove that a fact is absent
   from the whole manuscript. Report partial coverage; exhaustive tasks are a
   separate bounded workflow, not a top-k claim.
7. **Optional-layer failure:** code inspection finds valid raw planning can be
   suppressed when optional `memory_plan_add_briefs()` returns false in `chat.c`.
   Fix non-interference, but still revalidate raw scope/source eligibility.
8. **Fake compression:** current T1 rendering sends 15 full 64-character hashes
   for a four-input brief, before other metadata or summary. Validate the complete
   rendered token cost, not just the <=256-token summary payload.
9. **Unseen quotation:** two differently worded originals can produce the same
   paraphrased brief. A pointer/hash does not expose either wording. A model
   seeing only the brief cannot be required to reconstruct an exact unseen
   quotation. Route quote tasks to original text under the same budgets; retain
   the original V2 score and distinguish this future representational issue
   from the actual zero-brief V2 run.

An additional six-case diagnostic changes the **explicit author request** to ask
only for recall, preserving deliberate ambiguity and excluding brainstorming.
It removes the oracle-card arm. This is a capability control, not authorization
to silently rewrite ordinary author requests in production. Results:

- The quoted-original ambiguity case retains both accounts instead of naming
  a settled cause. Stronger task wording can help a particular representation.
- Native recall still introduces an unsupported location detail (“tower entrance,
  with Lena and Oren inside”) and sometimes treats deliberate ambiguity as a
  future author choice. It is not a universal cure.
- With the same originals and explicit recall request, adding the deliberately
  stale brief again produces the assertion that someone pushed the lamp. The
  response reinterprets the protected ambiguity as *who* pushed it, rather than
  whether it fell or was pushed. The “noncanonical; verify” label does not prevent
  this semantic change.
- Five of six outputs reach 128 tokens. Findings concern the tested output
  budget and observed prefixes, not unmeasured complete 1,024-token responses.

**Second claim damaged:** “explicit task instructions plus a noncanonical label
make derived state safe.” They help some cases but do not establish precedence
or semantic faithfulness. The first slice should not depend on this guarantee.

### Additional formal boundary: identical visible evidence, different answers

Consider two eligible source archives whose selected excerpt is the same:
“Let's use blue for the coat.” In one archive that is the final direction; in the
other a later omitted passage says “I changed my mind: use green.” If both plans
expose only the first excerpt, no answering model can reliably distinguish the
archives from the visible text. A source hash can prove identity, not reveal the
missing correction. A generic summary can faithfully compress the wrong slice.

This is why source units, whole-message ingestion receipts and contiguous job
frontiers do not by themselves prove **task-sufficient evidence**. Proposal/reply
and claim/correction adjacency need bounded packet tests. If the required closure
cannot fit, state the limitation or ask; never promise to scan arbitrarily far
until the answer looks settled.

### MODEL: invariants and decision gates

Let `R` be the eligible original-source packet, `B` an optional generated aid,
`Q` the protected current request and `C` the fully rendered context.

```text
authorize -> select bounded R -> render(Q, recent dialogue, R, optional B)
          -> exact count/admission -> generate -> report sources/limits
```

- `eligible(R)` is checked before and again at admission, independent of ranking.
- `valid(R) && invalid(B)` should preserve original recall after final validation;
  `!valid(R)` must not be rescued by an old brief or vector payload.
- `C` stays within the existing shared caps and preparation deadline. No new
  foreground generative call, unbounded neighbor walk or autonomous retrieval loop.
- Source handles bind only to the request's immutable host receipt, never to a
  model-created locator. Valid handle/quote does not prove semantic entailment.
- Brief processing coverage, useful retained information and retrieval coverage
  are distinct. Never silently advance published coverage on failure.
- Optional aids must earn benefit after full rendering, source overlap, retries,
  background work and failure coverage are charged.

### REFINE: a smaller first implementation

One bounded task-facing context assembler, existing authoritative storage and
short handles. Establish an original-only baseline first; optional aids must
pass both utility and poisoning/intent-preservation challenges before admission.
No new truth database, hierarchy, actor planner or automatic author-decision
promotion. Test packing first using the same sources; only then change selection;
only then change compression. This avoids crediting one component for another's
changes. If the model still fails the bounded task, stop adding scaffolding and
make the capability/resource tradeoff explicit.

## Scale attack and stop conditions

- At 0.1x, tiny turns can make brief JSON plus proof metadata larger than raw
  originals: choose the raw representation when it is cheaper and useful.
- At 10x, cold archival growth must not grow active packets, foreground reads,
  candidate counts or gen concurrency. Existing caps remain binding; partial
  coverage is explicit. Disk/index/backup capacity remains a separate measure.
- Across an hour/restart/retcon, source version, scope and author-intent fidelity
  must survive; a graceful visible gap beats a confident fabricated continuation.
- A larger/different model may be necessary if bounded native tasks remain
  unreliable. That is a separately authorized model/resource decision, not a
  silent fallback or an excuse to add more layers indefinitely.
- Exit this design cycle when the last adversarial pass adds no **new design
  requirement** and refinements remove machinery. It does not mean all empirical
  gates have passed. The remaining uncertain claims go into experiments below.

## Evidence provenance

- Cases: `build/memory-continuity-manifold-cases.json`, SHA-256
  `3e660aa1e574207a16aa56e4d51e36b5202a2d146aef4558d1f8ea0a1fa707da`.
- Raw events: `build/mb-n8yenzjl/events.jsonl`; manifest/props/script copied there.
- Generation log: `build/memory-continuity-manifold-probe.log`.
- Focused cases: `build/memory-continuity-focused-cases.json`, SHA-256
  `7914ba949534cf528ecb0f3bb71c499b180a623adf144ff833c231f9ca08f766`.
- Focused events: `build/mb-nb0y9sk0/events.jsonl`; log:
  `build/memory-continuity-focused-probe.log`.
- Earlier counterexamples: [MEMORY-010](../docs/MEMORY-010-quality-bedrock.md).
- Original intent: [MEMORY-002 §6](../docs/MEMORY-002-mvp-design.md).
- No frozen V2 fixtures, thresholds, results or production code changed.
