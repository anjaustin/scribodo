# Answer reliability manifold — SYNTHESIZE

## Outcome

**The central unresolved problem is reliable evidence-to-claim execution, not
simply whether the application remembers enough text.** The system can deliver
the correct original and the model can read it, yet the same model does not
reliably answer, cite, quote or abstain under the product's actual context.

This is a bounded diagnosis from a full [Lincoln Manifold Method](https://github.com/anjaustin/lmm)
pass with adversarial controls, not a claim of a discovered universal cure.

## What changed our understanding

1. **Capacity is not reliability.** Full-context Collins changes from a wrong
   447-token answer to a correct 20-token answer when only “Answer in one
   sentence.” is appended. It repeats exactly. Two equal-token non-directive
   suffixes remain wrong. No evidence/history removal or model replacement was
   necessary for this local success. The mechanism inside the network remains
   unmeasured.
2. **The prior false answer is not necessary for failure.** Two neutral histories
   preserve each message's 19/236 tokens and the full 5,267-token prompt. Both
   still fail Collins. Deleting author history is not an established remedy;
   earlier deletion results conflate length, position and content.
3. **Canonical recall is not the whole explanation.** In synthetic, explicitly
   rebound single-source controls, a one-sentence answer follows the changed
   gift, **a brass telescope**, rather than the novel's living. This demonstrates
   narrow source-following behavior, not general literary understanding.
4. **Stricter instructions and different decoding are not monotonic fixes.**
   Penalty 1.0 restores the answer word but produces a 2,048-token truncated,
   misattributed reply. Full-context quote-only also truncates. A correct word
   does not repair the complete reply.
5. **We must test the measurements too.** A 622-token raw synthetic quote response
   becomes a faithful 78-character visible quotation after actual native C
   cleanup. A raw-only failure label was wrong. Conversely the full-context
   failures remain visible after cleanup. Generation cost, visible content and
   source fidelity are different measurements.
6. **The tempting small fix fails transfer.** The unchanged one-sentence suffix
   works for opening/Collins but leaves a wrong income response, inaccurate
   citations on other factual replies, and a 2,048-token non-abstaining middle-
   name loop. It is **not approved as a global production prompt change**.

## The simpler working model

```text
SQL originals: authoritative bytes and eligibility
    ↓
Vector/lexical search: candidate discovery, not truth or answerability
    ↓
Planner: bounded eligible evidence, with explicit partial coverage
    ↓
Model: fallible proposal of answer claims, not promotion into canon
    ↓
Native cleanup: visible presentation, not factual correction
    ↓
Acceptance: current-question relevance + support + entity/source binding
            + faithful quotations + appropriate uncertainty + completion
```

Do not collapse these into “memory works” or a phrase-presence score. Do not
multiply component pass rates into a product readiness estimate.

The income original's fused-rank-9 loss is a real designated-passage coverage
miss. It is not yet an isolated cause of the wrong income answer: a later admitted
passage also contains the correct range. Missing that designated original, a
wrong answer, and a successful hypothetical repair are three different claims.

## Decisions

- **No new memory tier, global history deletion, model swap, repetition-penalty
  change, or one-sentence production policy is justified by this pass.**
- Preserve author intent/authority, consent, eligible source bindings and the
  distinction between originals, prior assistant claims and proposed writing.
- Keep exact source/claim fidelity separate from answer brevity. A short wrong
  answer is still wrong; a correct answer without a citation has not exercised
  citation reliability.
- Treat exact-source checks as deterministic integrity checks, not semantic
  entailment or answerability proofs. A second model sharing the generator's
  mistake is not an independent acceptance oracle.
- Keep the previously measured numerical-parity discrepancy open. Shared-prefix
  semantic errors refute a backend-only explanation, not every backend defect.

## Next executable cycle

Run in this order, with prospective protocols and complete replies preserved:

### 1. Isolate income selection versus task execution

Use actual captured income request 1, same endpoint/model/slot and fixed decoding.
Compare its exact packet with a diagnostic packet replacing the later income
source with the opening original, **carrying the correct source binding with it**.
Keep all author history and other sources. Record changed token positions rather
than claiming an identical-length comparison. Pair each packet with the same
baseline and narrow factual-task condition. Verify final rendered inputs and
locators, not just the answer phrase.

**Decision:** promote a selector experiment only if the exact passage admission
improves and complete answers survive fresh task cases; otherwise selection and
execution remain independent work. Do not put oracle passage knowledge into a
production query/ranker.

### 2. Challenge a minimal task contract, especially at uncertainty boundaries

Create **new** source-bound questions before testing: single fact, two entities
with different values, explicit author correction, contradictory dated drafts,
unavailable fact, and a creative/speculative request. Compare current behavior
with a candidate factual contract while leaving source policy unchanged. No
silent classification may turn author speculation into fact or suppress a
requested draft. Start with explicit task conditions rather than an unvalidated
automatic router.

Review each visible claim against eligible sources. Keep fact selection,
unsupported additions, quotation spans, citation binding, contradiction handling,
abstention and completion as separate fields. An unavailable fact requires an
honest statement about supplied evidence, not a global assertion of absence or
canonical guessing. Do not use this novel's answer vocabulary as the pass oracle.

**Decision:** reject the candidate on boundary-task failure; do not add successive
instructions merely to make Collins pass. If this pinned model cannot execute
the contract reliably on fresh cases, test a model/training alternative under
the same frozen inputs rather than expanding memory indefinitely.

### 3. Resolve numerical discrepancies with matched-prefix backend controls

At the previously measured discrepant positions, compare llama CPU, llama Metal
and official CPU F32 on **identical teacher-forced token prefixes**. Freeze
weights, templates, token IDs, cache/batch settings and tolerances. Distinguish
arithmetic divergence from sampling divergence and semantic correctness. Run
serially; do not waive the existing tolerance because first divergences are ties.

## Acceptance and stop rules

- Production behavior changes only after the relevant counterexamples and fresh
  questions pass **complete visible replies**, with raw streams retained.
- A pass in one task class does not certify drafting, contradiction resolution,
  absence handling, retrieval coverage or an entire vector collection.
- If tests contradict the model, return to REFLECT with the counterexample;
  do not force a single-cause explanation or invent a readiness percentage.
- This pass stops with the measured transfer failures intact. Its outcome is a
  sharper, executable investigation and rejected false remedies—not a release.

## Evidence and phase records

- [RAW evidence and uncertainties](answer-manifold_raw.md)
- [NODES and tensions](answer-manifold_nodes.md)
- [REFLECT and revised causal boundaries](answer-manifold_reflect.md)
- [TURBOCHARGER protocols, counterexamples and results](answer-manifold_turbo.md)
- Prior [live-path audit and independent runtime comparison](answer-bedrock_live.md)
- Local complete artifacts: `build/am1/`, `build/amc1/`, `build/amt1/`.

No production source or CI changes were made. Diagnostic changes are uncommitted.

Final verification (`build/answer-manifold-verification.json`): **20/20** model
arms have matching official/provider template bytes and prompt counts, zero
cached prompt tokens, and retained native-visible transformations; **31/31**
no-model answer tests and four native text smoke controls pass. All 169 production
source hashes still match the live baseline. `git diff --check` is clean; no
Scribodo/llama-server process remains. These checks qualify the investigation,
not the factual correctness of all 20 replies.

## Reopened by user: audit the caller before interpreting capacity

The user emphasizes the model's capability and suspects missing plumbing. The
shared-reference results do not clear the caller: both engines received the
application's same composed messages. The diagnosis above must not be read as
proof of inadequate model capacity or a reason to replace it.

Plumbing-first controls are now specified in turbo cycles 5-7: strict CPU/Metal
and KV-type arithmetic localization; exact-message vendor-core sampling/thinking
comparisons; and a paired reasoning/output-budget test. Two control defects were
found already: `-ngl 0` alone does not certify CPU-only operation, and three
diagnostic budget guards counted `BatchEncoding` fields rather than tokens.
Both are remediated in diagnostic code, with failed/invalid controls retained.
The production C token counter is not affected by that Transformers API bug.
Neither finding is yet the demonstrated cause of the product's answer failures.

Original response observations remain preserved, including independent provider
token counts and subsequent template checks. No prompt/model/production policy
has been changed to conceal a discrepancy. The strict matrix runs under
`build/ap2/`; contract/budget runs are queued to run serially afterward.

### Reopened pass: completed observations so far

- Strict six-prefix arithmetic matrix completed: maximum reference-top-ten gaps
  .95103 Metal/BF16 KV versus .09977 strict CPU/BF16 KV. F32 KV is not uniformly
  better. These are diagnostic greedy-derived prefixes, **not proof that a
  specific discrepancy caused a native sampled answer**.
- The original native Collins/middle-name raw failures reproduce exactly in a
  fresh cold server on their captured messages. Vendor-core sampling plus
  thinking fixes Collins' source binding on this case, but fabricates an absent
  middle name. Thinking only exhausts 2K without a final answer.
- Output-limit pair, same one-slot 16K context: native middle-name generation
  stops naturally at **2,359 tokens** when allowed to continue beyond 2,048.
  Its complete 2,048-token prefix is identical to the original, but the later
  reasoning-close tag lets native cleanup reduce 9,525 raw characters to 793
  visible characters and expose a final core abstention. Citation inaccuracies
  remain. Vendor-both's false name is unchanged by the larger cap.
- Actual consumption plus reserve is **5,529 + 2,359 + 256 = 8,144**, within the
  native 8K slot. This motivates an unchanged-slot 2,407-token-cap confirmation,
  not silently raising a user's configured limit.

Next bounded controls are running serially: original-slot remaining-budget
confirmation (`build/pr1/`); larger budget for the actually truncated native-
sampler thinking-only condition (`build/pt1/`); strict CPU full native replies
(`build/pq1/`). See turbo for prospective protocols. No production policy is
changed, and model-capacity claims remain suspended.

### Follow-up results qualified

Original-slot confirmation **pr1 passes the cap-isolation control**, not answer
fidelity: 2,407 allowed tokens finish at 2,359, exactly matching pb1's completed
message and 793-character native-visible output. Official/provider templates,
counts and cold accounting match across pr1 and pt1.

Thinking-only with sufficient diagnostic budget **does not fix Collins**: pt1
finishes at 6,226 tokens and still answers shelves/approval. Middle-name finishes
at 5,012 with core abstention but false supporting assertions. Both extend their
2K reasoning prefixes. Reasoning-channel handling and factual fidelity remain
separate obligations; completion alone is not success.

Strict-CPU full-reply control pq1 is **incomplete**, not a negative quality result.
The first request was generating steadily and logged 2,025 tokens when the
600-second nonstreaming transport allowance expired. No answer was captured,
and the second question was not submitted. Preserve its evidence; a corrected
transport control is required before CPU/GPU whole-reply conclusions.
