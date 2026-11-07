# LMM SYNTHESIS — preserve the author's working situation

**Status: recommendation and experiment plan, not implementation approval or a
quality-gate pass.** Production code, consent, model identity and frozen V2
evaluation remain unchanged. Memory/T1 remain default-off; T2 remains deferred.

## The clean cut

**Build reliable continuity around the author's present task, not a growing
warehouse of inferred facts. Grounded storage does not guarantee grounded
behavior.**

The writing promise is: *remember what we are doing without taking over what
the book means*. That includes accepted/rejected possibilities, current intent,
the next action, and uncertainties the author intentionally wants to preserve.

## What the manifold added beyond the bedrock investigation

| Finding | Evidence | Consequence |
|---|---|---|
| The implemented T1 target drifted from continuity to fact enumeration | MEMORY-002 §6 calls for continuity/decisions/open questions; `memory_brief.c` requests a `facts` array | Repair the task definition before changing JSON limits or adding tiers |
| A correct working-state card is not a reliable cure | New hand-written oracle-card controls sometimes induce card-writing, outlines or unsupported detail | Do not start by building a semantic state database |
| “Not yet decided” differs from “must remain unresolved” | Narrator choice versus deliberately ambiguous bell/fire cases | Evaluate these separately; a generic TODO/open-question list can change author intent |
| A noncanonical brief can poison correct originals | Stale fire-cause brief biases both ordinary and explicit-recall responses toward a pushed lamp | Warnings and source membership do not establish semantic precedence; T1 needs poisoning tests |
| More explicit recall instructions help only some cases | Six focused controls preserve some ambiguity, but still show invention and stale-brief influence | Do not make the author repeat stronger instructions every turn; establish the model/task envelope |
| Chunks are not complete evidence episodes | A proposal and a ratifying reply, or a claim and omitted correction, can be split | Test bounded adjacent-original packets; expose missing context rather than infer a final decision |
| Optional derived work is currently coupled to original recall | `chat.c` sets `available` from the optional brief call even though the planner stages failures without disturbing raw units | Reproduce and fix raw-source fallback independently of semantic changes |
| Small summary text is not necessarily small context | Four-input T1 rendering carries 15 full hashes, 960 hex characters, before other metadata | Keep full proof receipts in host code; measure total rendered context with short handles |
| A summary is not an original quotation | Differently worded originals can have the same paraphrase; an opaque pointer does not reveal wording | Route quote/dispute tasks to original excerpts; evaluate continuity retention separately from quoteability |

### Empirical limits

The new study comprises 16 base and six explicit-recall diagnostic generations,
four synthetic continuity scenarios, one pinned local model/runtime, and a
128-token output allowance. Eight base and five focused outputs hit the limit.
Observations are qualitative, not a new accuracy score. Hand-written oracle
cards are upper-bound controls, not an extractor. Prompt lengths differ; cost
and extra card information are not hidden. No ordinary/held-out writing-session
success, sustained-use result or universal model limitation is claimed.

## Recommended shape: one bounded working-context packet

```text
current author request — protected
        +
bounded recent dialogue with speaker/order intact
        +
query-relevant original evidence, grouped into bounded episodes where possible
        +
optional continuity aid ONLY after it earns utility and non-poisoning evidence
        |
host binds short handles to immutable source/version/range receipts
        |
exact token fit + final scope/source admission
        |
existing local model -> response + as-of Sources/coverage
```

This is a refinement of the existing native planner, not a new sidecar, agent
loop, graph database or memory hierarchy. “Working context” is a per-request
view over authorized material, not a new canonical store.

### Host versus model responsibilities

**Host can enforce:** consent/scope/provider boundaries; source identities,
versions and eligibility; speaker/order metadata; exact quotation and handle
binding; protected current request; resource bounds; immutable as-of receipts;
no automatic manuscript/canon mutations.

Short handles are per admitted request/reply. Their complete source mapping and
proof metadata must remain in an immutable, bounded as-of receipt, separate from
the compact model-facing text—not be discarded or rebuilt later from current
SQL rows. Build both after final optional-context fitting so handle numbering
cannot drift when units/briefs are dropped. Keep both the existing model-reference
and 16 KiB Sources-receipt caps; a cheaper prompt is not permission for an
unbounded receipt or additional source access.

**Model must be tested for:** correct interpretation, rejection versus proposal,
current intent, deliberate ambiguity, task execution and faithful response.
Neither a valid citation nor a generated status label proves entailment.

Historical author directions remain evidence about earlier intent, not privileged
system instructions. The current author can change course. A character's claim
or quoted manuscript instruction is not an author commitment. When scope or
version is genuinely ambiguous, clarify rather than silently select canon.

## Options considered

| Option | Position |
|---|---|
| Raw retrieval + current native tail, no generated summaries | Required control and viable simplest outcome if it meets continuity gates |
| Short handles + deterministic history/evidence packing | **First candidate:** limited change; preserves existing stores and bounds; grouping still needs adjacency/authority tests |
| Query-relevant, source-backed episode snippets | **Next candidate:** relationships without inventing a state ledger; all neighbors count against existing caps |
| Extractive episode bookmark / tiny typed T1 aid | **Conditional experiment:** compare against original-only packets, including rendered cost, source overlap and poisoning |
| Persistent semantic decision/branch ledger | Defer: extraction errors can become durable false authority; source revocation and branch semantics expand scope |
| Larger/different model | A legitimate possible capability decision, but separately authorized and measured under local/BF16/resource constraints |
| T2/checkpoint hierarchy | Defer until T1 demonstrably earns its cost; it cannot fix a wrong semantic target |

## Execution sequence

### E0 — isolate optional-layer failures (deterministic correctness)

Reproduce a malformed/ineligible brief with otherwise valid original candidates
through native ChatSession. Require original recall to survive a brief-only
failure, but not a source/scope/consent failure. Final source admission remains
mandatory. Keep this repair separate from changed selection/rendering so its
effect is attributable.

**Exit:** fail-before/pass-after integration regression, remote/cancel/scope
regressions, normal/DB/sanitizer/UI validation. No semantic success claimed.

### E1 — establish the bounded native capability envelope

Use actual ChatSession rendering/admission and the production persona. Separate
ordinary alternating histories from consecutive-user, repetitive, long-message,
correction and partial-source stress. Test:

- exact detail and quotation, including Unicode;
- accepted versus merely proposed/rejected ideas;
- resume action, pending choice and deliberately preserved ambiguity;
- retcons/versions and current-author overrides;
- partial evidence and unanswerability;
- recall versus brainstorming/drafting with the same prior material.

For recall, invented details are failures; for requested drafting, novelty is
expected but claimed prior decisions and author constraints must remain faithful.
Do not evaluate every writing task as literal fact extraction.

Compare original renderer with short-handle/packing changes on the **same selected
sources** first. Native configured reply allowances and lower-budget stress must
be separate preregistered conditions, matched across arms; do not quietly replace
V2's allowance. Never manufacture assistant turns, silently rewrite the author's
current request or discard the adversarial user-only history to obtain green.

**Exit:** credible ordinary and stress performance across task/authority strata,
not one improved mean score. If simple bounded native cases remain unreliable,
stop adding memory layers and make the capability/no-go decision explicit.

### E2 — test evidence episodes before semantic state

Within the same allowed candidate/source budgets, preserve adjacent proposal /
reply and claim / correction context where available. Compare packing separately
from changed source selection. No unbounded search for semantic closure: if the
relevant episode cannot fit or is missing, expose partial context and clarify.

The current 32 candidates, eight original excerpts, shared 16 KiB/4,096-token
optional reference budget and 500 ms experimental preparation budget remain
binding. No extra foreground generative summary or autonomous retrieval loop.
Eligibility always precedes ranking; new packet assembly does not grant cross-project,
cross-conversation, remote or dirty-document memory access.

### E3 — make a continuity aid earn admission

Only after the original-only path is reliable, compare no aid, an extractive
episode bookmark and a small typed aid over original sources. Candidate fields
may distinguish next action, author commitment/rejection, pending choice,
deliberate ambiguity and persistent constraint; **do not implement all fields
or declare them reliably extractable merely because they are named here**.

Keep 256 summary tokens and existing input/generation bounds for this experiment.
Use an achievable structure; count the entire rendered aid, handles and overlap,
not just JSON payload. A brief acting as an orientation supplement is not the
same claim as a compression replacement. Record which role is being tested.

For quote/dispute tasks, select the necessary original excerpts within the same
source/candidate/token limits. Do not quietly inject all brief input bodies or
expect the model to reconstruct unseen wording. For continuity tasks, test
intent/relationship retention and explicit source binding; that is not a license
to call a generated paraphrase an original quote or to weaken the frozen V2 gate.

Measure: completed output, useful retained relationships, background tokens/
time/retries, processed coverage, usable coverage and rendered context gain.
An empty valid brief is not proof that important intent survived. A source-rich
but wrong brief is worse than no brief. Add deliberately wrong/stale/misattributed
aid challenges with correct originals present. A label saying “not canon” is
not a pass condition. Keep failed publication/coverage visible; no silent holes.

**Exit:** improvement over the simplest original-only arm after cost and harmful
influence are charged. If absent, leave T1 off; do not compensate by adding T2.

### E4 — preregister fresh held-out native acceptance

Freeze new unseen cases, rendering/protocol versions, model/config/budgets and
scoring before inference. Keep V2 and all development probes as negative and
diagnostic evidence; do not reuse them as fresh held-out wins.

Report axes separately, then apply a conjunctive release decision:

1. source eligibility/privacy/provenance;
2. selection and task-sufficient evidence limitations;
3. factual/quotation fidelity;
4. proposal/decision/rejection and author-intent fidelity;
5. pending versus deliberately unresolved state;
6. current-task completion, not unsolicited world-building during recall;
7. citation/handle/schema validity where required;
8. generation completion/conciseness and total cost;
9. ordinary versus adversarial strata and harmful-aid influence;
10. native responsiveness, sustained real-model use and restart/revision behavior.

Thresholds remain experimental and must be frozen before the held-out run. No
aggregate improvement can hide a privacy/authority violation or systematically
poor subgroup. T2, scope expansion and default-on remain independent decisions.

## Open questions, not hidden commitments

- Does native source packing solve enough without generative summaries?
- Can a semantic aid improve orientation without poisoning original evidence?
- Which short-source cases should stay raw because serialized briefs expand them?
- How much neighborhood context is needed for valid intent interpretation within
  the existing source limits? Which tasks must ask rather than claim completeness?
- Can this checkpoint reliably separate recall from creative extension, or is a
  different capability/workflow necessary? Numerical runtime checks do not answer it.
- How should an author intentionally change, protect or reject a remembered
  constraint without creating an automatic canon-management feature?

## Phase records and evidence

- [RAW](memory_continuity_raw.md)
- [NODES](memory_continuity_nodes.md)
- [REFLECT](memory_continuity_reflect.md)
- [TURBOCHARGER, probes and provenance](memory_continuity_turbo.md)
- [Bedrock diagnostics](../docs/MEMORY-010-quality-bedrock.md)

Method: installed LMM v2 plus the repository revision already pinned by
MEMORY-002: [LMM at 55719c0](https://github.com/anjaustin/lmm/blob/55719c0a2308d1a3f29ca656ed7b6287be33bca2/LMM.md).
The newly supplied `anjauastin/lmm` URL returned 404. No method-source substitution
or experimental result is hidden. The author's pre-existing sample edit remains
untouched. This pass changes diagnostic tools and investigation records only.
