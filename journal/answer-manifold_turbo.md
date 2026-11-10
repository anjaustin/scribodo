# Answer reliability manifold — TURBOCHARGER

## Cycle 1: challenge the diagnosis using existing counterexamples

| Claim attacked | Adversarial evidence | Result |
|---|---|---|
| Better retrieval is the complete answer | Collins' exact source is admitted first and the answer is wrong | Broken |
| A correct answer word establishes fidelity | Single-unit Collins names living but invents quotation/meaning | Broken |
| All wrong answers are backend defects | Independent shared prefixes contain source/entity errors | Backend-only explanation broken; extra backend faults remain possible |
| History removal proves semantic poisoning | Removal also changes 267 token positions | Not established; requires matched-token controls |
| Single-source failure proves inability to identify the fact | Single-source response names living correctly | Overbroad incapacity claim broken; claim elaboration remains defective |

This is a historical-failure attack using this product's retained experiments,
not a claim about other products. The strongest alternative to a broad model
incapacity diagnosis is a task-contract/decoding mismatch: the generator can name
the fact but continues with unsupported explanation.

## Model under test

```text
author question + eligible originals
    -> candidate discovery -> bounded admission -> final serialized messages
    -> template/tokens -> arithmetic logits -> sampling/penalties
    -> generated claims -> presentation -> acceptance

Claim obligations = {answers current question, supported by eligible evidence,
                     correct entity/source binding, faithful quotation,
                     permitted speculation explicitly identified}
```

No multiplication of pass percentages is meaningful here: obligations are
dependent, and failures can coexist. A candidate may be genuine but irrelevant;
an output may contain the right fact but make additional unsupported claims.

**Refinement:** replace the vague category “model failure” with observable
fact-selection, quotation, attribution and elaboration outcomes. This adds no
new product component or persistence tier.

## Cycle 2: predeclared fresh controls

Runner: `tools/answer_manifold_probe.py`; destination `build/am1/`. Same actual
Collins request, same model/server binary, same chat endpoint, seed 42, cold
generation, 8K slot and 2,048-token cap. Temperature zero throughout. Source
packet and source eligibility remain unchanged except the explicit oracle arm.

| Arm | Intervention | Discriminating observation |
|---|---|---|
| baseline | None beyond common cold greedy setup | Repeat complete full-context failure under this server |
| penalty_1 | Repeat penalty 1.05 → 1.0 only | Separates penalty from earlier route/cap differences |
| neutral_weather | Replace history contents, match each message and full-template token counts | Tests history-content sensitivity without shorter input |
| neutral_room | Second independently worded matched-token history | Tests whether first neutral result was wording-specific |
| one_sentence | Append “Answer in one sentence.” only | Tests overgeneration hypothesis without removing evidence/history |
| quote_only | Request one exact original sentence only | Tests extractive fidelity, not free-form understanding |
| oracle_one_sentence | Existing intact single-unit control plus same one-sentence constraint | Tests whether single-source elaboration persists under a narrow contract |

Arms and prompt token counts are frozen before inference. No adaptive instruction
search or automatic pass-by-keyword is permitted in this cycle. Complete replies
will be reviewed for core fact, added claims, quote fidelity, citation binding
and termination separately. An omitted citation is not a correct citation; a
constraint that removes citation obligations has a narrower contract.

### Scope/scale attacks

- At 0.1× context, the prior oracle still invented a definition: less input is
  not automatically safer.
- At larger corpora, preserving all relevant relations under a fixed admission
  budget is a separate unmeasured problem; no scale claim is authorized.
- Factual extraction is not creative drafting, contradiction resolution or
  absence detection. Any positive result here must be challenged on those task
  boundaries before production promotion.
- A near-tie numerical divergence can change later prose, but cannot explain
  an error in an identical shared prefix. Conversely a shared-prefix error
  does not waive the measured numerical-parity failure.

### Exit condition

End this bounded pass after reviewing all predeclared complete responses and
refining the model once. If results change the premise, loop back explicitly;
do not claim global convergence or keep adding prompt patches to this fixture.

## Results

Cycle 2 complete: `build/am1/results.json`. All seven template renderings match
the official tokenizer byte-for-byte; official/provider prompt token counts
match. The baseline complete response equals the earlier `build/ala1/` full
response. All requests report zero cached prompt tokens.

| Arm | Prompt / output tokens | Finish | Complete-reply observation |
|---|---:|---|---|
| baseline | 5,267 / 447 | stop | Wrong: home visit, dinners and shelves instead of living |
| penalty_1 | 5,267 / 2,048 | length | Names living but adds wrong inheritance/source claims and loops |
| neutral_weather | 5,267 / 411 | stop | Wrong: church/social invitation; adds wrong recipient/relationship claims |
| neutral_room | 5,267 / 448 | stop | Wrong: invitation; adds Pemberley and sibling claims |
| one_sentence | 5,272 / 20 | stop | Correct: “Lady Catherine de Bourgh has given Mr. Collins a living (a job or position).” |
| quote_only | 5,301 / 2,048 | length | Violates contract, misquotes/misattributions and repeated searching |
| oracle_one_sentence | 710 / 30 | stop | Names living with a supported paraphrase of Collins' freedom from minor money concerns |

### Claims broken and model refinement

- **False prior answer is necessary for the Collins failure:** broken by both
  token-matched neutral histories. This does not prove history contents never
  matter; it rejects this necessary-cause claim. Earlier history deletion is
  still not an isolated demonstration of semantic poisoning.
- **No repetition penalty repairs the answer:** broken at complete-reply level.
  A correctly named fact appears, but completion/fidelity fail. The one-field
  pair does establish a decoding effect on these actual inputs.
- **More explicit evidence rules are monotonically safer:** broken. Quote-only
  is substantially less successful than a five-token one-sentence constraint
  on this input. This is a measured counterexample, not a general comparison
  of every evidence protocol.
- **The model cannot identify the fact with full native context:** broken by
  the one-sentence arm. No source or author history had to be deleted.

Refined model: response instructions and decoding influence **which fact is
selected and how many claims are made**, not merely the prose after a fixed
answer. This is an external behavioral result; the internal attention or neural
mechanism was not measured. Keep the simpler distinction between evidence
delivery, task execution, and complete-reply fidelity.

The one-sentence result alone does not isolate instruction meaning from adding
five tokens at the end of the request, and it does not establish transfer to
other questions. Cycle 4 below is a confirmatory falsification, not more prompt
search until this fixture passes.

### Predeclared review rubric

- **Core answer:** correctly identifies the living bestowed on Mr. Collins, not
  Elizabeth's age, dinner invitations, furniture or a different recipient.
- **Added factual claims:** inspect every additional definition, location,
  relationship and event; an unrequested false assertion is still a failure.
- **Quote fidelity:** quoted support must be a contiguous span of an eligible
  original after removal of presentation wrappers only. A stitched paraphrase
  is not a verbatim quotation even when its core proposition is true.
- **Citation binding:** every explicit SOURCE/document label must identify the
  supplied original containing the claimed support. No citation means this
  obligation was not exercised, not that citation reliability improved.
- **Completion:** retain finish reason, raw complete content and output token
  count. Length-limited answers cannot receive a complete-reply pass.
- **Contract scope:** a successful one-sentence answer does not demonstrate
  explanatory understanding; a successful quote does not establish conflict
  resolution, reliable retrieval or absence detection.

No aggregate readiness score or unmeasured confidence percentage will be derived.

## Cycle 3: predeclared challenge to a positive result

Before seeing cycle 2 outcomes, reserve this counterexample: a correct answer on
an iconic novel may come from prior knowledge rather than reading the supplied
passage. Compare the same single-unit passage with `a living` versus `a brass
telescope`, under the same one-sentence and quote-only contracts. Both variants
must be explicitly bound as synthetic diagnostic data, without falsely retaining
the original document's revision/hash metadata. Do not edit SQL, vector points,
the manuscript, or production sources. The expected telescope answer tests the
source-following claim; it does not measure ordinary manuscript retrieval.

Run serially after cycle 2. Keep both complete responses and record any narrower
task phrasing needed to distinguish “according to this text” from canonical novel
knowledge. This cycle cannot certify explanatory understanding of “living.”

### Cycle 3 results

`build/amc1/results.json`: both one-sentence arms stop at 21 tokens. The living
control answers living; the altered-source control answers **a brass telescope**.
This supports source-following capacity under this narrow synthetic contract,
not merely retrieval of the canonical novel answer.

**Correction after testing the review itself:** the initial raw-only review
classified both quote contracts as failures. That classification omitted the
product's native thinking cleanup. `living_quote` emits 622 tokens/2,692 raw
characters, but ends in an orphan `</think>`. The actual native C functions
`llm_split_thinking` and `llm_strip_speaker_labels` leave **78 visible characters**:

> “Lady Catherine de Bourgh,” she replied, “has very lately given him a living.”

Its content is a contiguous source span after removing the outer quotation
wrappers, satisfying the predeclared presentation-wrapper allowance. Exact raw
byte equality with a source substring is false: it closes the ongoing dialogue
after the sentence. Both checks are preserved in `quote-binding-controls.json`.
This is a correct narrow visible quotation, with substantial hidden generation,
not a 622-token visible failure. `telescope_quote` still returns the five-token
answer “A brass telescope.”, not the requested original sentence.

All four finish with `stop` and zero cached tokens. The source text's interrupted
dialogue makes sentence-boundary copying nontrivial; these results do not
demonstrate that all possible extraction formats fail.

### Cycle 3b: measurement-model adversarial check

Compile **unchanged** `src/llm/llm_text.c` as a diagnostic shared library; apply
the actual product text functions, not a guessed Python imitation, using
`tools/answer_manifold_visible.py`. Four native smoke controls pass (plain UTF-8,
orphan close, matched thinking tags, speaker cleanup). All cycle 2 replies remain
unchanged; only cycle 3's living_quote changes. Source/library/result/tool hashes
are recorded in each `native-visible.json`.

**Claim genuinely broken:** “a long raw self-correcting response necessarily
means a long incorrect visible answer.” It does not. Earlier generation costs
and raw contract behavior remain measured; visible correctness must be evaluated
after the product transformation. This refinement changes an outcome rather
than merely adding a disclaimer.

## Cycle 4: final predeclared falsification before synthesis

Freeze two non-directive suffix controls, each with the same added rendered
token count as “Answer in one sentence.”: “This is a question.” and “The question
ends here.” Verify counts with the official tokenizer before inference. Re-run
the one-sentence Collins arm as a positive replication in the same owned server.
Then apply the unchanged one-sentence suffix to the other six captured actual
requests, retaining their original histories/evidence. These are non-tuned
cross-question controls, **not fresh unseen manuscripts or independent samples**.

If neutral suffixes also restore the answer, weaken the instruction-specific
claim. If other questions remain wrong, reject a global one-sentence remedy.
Do not change the suffix again in this pass. Use the same full native reply cap;
do not turn early truncation into a pass. Stop after these controls and synthesize
bounded findings, including remaining failures and the next discriminating work.

Input qualification before outcome review (`build/amt1/input-controls.json`):
both neutral suffixes and the positive replication render to **5,272 tokens**.
All nine arms retain the exact captured history and evidence; only the recorded
suffix is appended to the last user content, with common cold greedy settings.
The other six rendered prompt counts are 5,413 / 5,801 / 5,673 / 5,524 / 5,436 /
5,534. Every arm reserves the entire 2,048-token reply allowance within 8K.

### Cycle 4 results and final refinement

`build/amt1/results.json`: all nine template byte comparisons and official/provider
prompt-count comparisons pass. The Collins positive replication matches the
entire earlier 20-token response exactly. All provider responses are cold. Native
C text cleanup leaves all nine replies unchanged.

| Arm / question | Output tokens | Finish | Complete visible reply observation |
|---|---:|---|---|
| Neutral suffix: “This is a question.” | 491 | stop | Wrong age answer attributed to Collins |
| Neutral suffix: “The question ends here.” | 490 | stop | Same wrong age answer |
| Collins one-sentence replication | 20 | stop | Correct living/job-or-position answer, exact replication |
| Opening scene | 43 | stop | Correct let-at-last, wealthy northerner, Michaelmas answer |
| Income | 34 | stop | Does not give annual income; gives inherited capital and unsupported purchase-Netherfield claim |
| Colonel age | 257 | stop | Correct thirty; ignores sentence constraint and adds unsupported/stitched quoted material |
| Georgiana | 292 | stop | Correct Ramsgate/two men-servants; ignores constraint, cites the servant passage as SOURCE 5 instead of SOURCE 3 |
| Camp | 73 | stop | Correct Brighton; falsely calls spoken dialogue Lydia's letter and cites SOURCE 3 instead of SOURCE 1 |
| Missing middle name | 2,048 | length | No clean abstention; visible speculative loop with invented relationships |

The equal-token suffix counterexamples weaken a pure added-length explanation
of the Collins result. They support a content-specific task-instruction effect
on this captured input, not a general theory of internal model computation.

**Claim broken:** “one sentence is the remedy.” It is not. It can yield a short
wrong answer, can be ignored, and does not enforce attribution or abstention.
No further prompt variants were searched in this pass.

**Final model:** integrity, evidence coverage, task execution and complete visible
claim fidelity are separate obligations. Some narrow source-following capacity
exists; neither broad incapacity nor reliable execution follows from that fact.
The new evidence removes the need to hypothesize that the previous false income
answer is a necessary cause, or that deleting history is required to answer
Collins. It leaves a simpler task/evidence-to-claim reliability problem alongside
the independently measured selection coverage and numerical runtime questions.

### Exit and limitations

Stop after the 20 predeclared model arms (7 mechanism, 4 counterfactual,
9 final falsification). This is a bounded pass, **not global convergence**. The
last controls found transfer/abstention failures; those require a new scoped
cycle, not silent continuation of prompt tuning. All complete raw responses,
native-visible transformations and template controls are retained. These are
one novel's captured requests plus synthetic derivatives, not independent
cross-corpus product acceptance. No accuracy or readiness percentage is inferred.

## Reopened cycle 5: plumbing-first arithmetic localization

User steering explicitly challenges a capacity interpretation. Keep the pinned
model and actual serialized messages. `tools/answer_plumbing_arithmetic.py`
freezes six points from `build/alp1/`: position zero, worst measured gap and first
greedy divergence for each of income/Collins. This is retrospective selection
for fault localization, not a fresh model-quality sample.

At each identical prompt-plus-candidate prefix, measure one unpenalized next-token
distribution under four serial owned runtimes: Metal/BF16 KV, CPU/BF16 KV,
Metal/F32 KV and CPU/F32 KV. Preserve batch 256, two 8K slots and flash attention
off. Check cold token accounting and actual token/distribution alignment. Compare
each with the frozen full-forward official CPU F32 distributions and with the
original incremental llama output. F32 KV is diagnostic only; no precision policy
is silently changed in the product. Artifacts: `build/ap1/`.

Interpretation rules: a Metal-only discrepancy implicates backend arithmetic;
a KV-type effect implicates cache rounding/storage; a cold-versus-incremental
effect on the same backend warrants state/batch investigation. None alone proves
the entire original quality failure is fixed. Do not waive the discrepancy or
infer model incapacity before these controls return.

### Control defect discovered before interpreting the matrix

`-ngl 0` is not sufficient to certify CPU-only execution in this build. The
source initializes available device backends, and `ggml-backend.cpp:1375-1384`
allows operation offload from host weight buffers; Metal's offload rule accepts
matrix operations at batch size >=32 by default (`ggml-metal.cpp:850-855`,
`ggml-metal-device.m:1285`). This establishes an offload possibility, not a
measurement that every operation in the old run actually used Metal.

The owned `build/ap1/cpu_bf16/` server was terminated deliberately; its runtime
and parent exited nonzero, and partial logs/results are preserved with the
original runner and `CONTROL-LIMITATION.txt`. Do not interpret ap1 as a pure
CPU/Metal factorial. Restarted in **`build/ap2/`** with CPU flags explicitly
including `--device none --no-op-offload --no-kv-offload`, and verbose backend
logging for every configuration. A regression test checks all three CPU guards.

### Strict matrix outcome (`build/ap2/`)

All 24 one-token controls completed, with cold accounting and matching returned
token/probability IDs. Strict CPU logs show zero GPU layers and only CPU compute
buffers (CPU/BLAS backends); device/op/KV offload are explicitly disabled.
Every official top-ten token is covered by each candidate's recorded top-64.

| Live case / candidate position | Metal BF16 KV | Strict CPU BF16 KV | Metal F32 KV | Strict CPU F32 KV |
|---|---:|---:|---:|---:|
| Income / 0 | .08810 | .02110 | .07215 | .01758 |
| Income / 259 | **.95103** | .09977 | **.27238** | **.42199** |
| Income / 312 | .06208 | .02419 | .02038 | .02067 |
| Collins / 0 | .06372 | .03051 | .03363 | .02865 |
| Collins / 310 | .02900 | .03161 | .01955 | .01758 |
| Collins / 339 | **.22459** | .04455 | .07331 | .01645 |

Entries are maximum reference-top-ten log-probability gaps on **identical token
prefixes**, not quality scores. Strict CPU/BF16 KV is below the pre-existing .12
bound at all six selected points. Metal/BF16 exceeds it twice; both F32-KV paths
exceed it at income/259. Higher KV precision is not a monotonic remedy here.
Two Metal/BF16 argmax disagreements and one CPU/BF16 disagreement occur at the
preselected near-tie divergence points; both F32-KV paths match argmax at all six.
That does not certify F32-KV numerical parity or any complete-answer behavior.

At income/259, reference top-one probability is .94604 and Metal/BF16 assigns
it .96738. The largest gap is on reference-rank-4 token 11652: probability .00653
versus .00252. All configurations still select the same top token there. Preserve
this probability scale (`probability-scale.json`) rather than equating a .951
log-probability discrepancy with a .951 error probability or a wrong answer.

The original incremental Metal/BF16 gap at income/259 was .64245 versus the
fresh-prefill .95103. This establishes computation-path sensitivity but does not
isolate recurrent state corruption: prefill/decode batch shape and activation
rounding also differ. The matrix localizes a backend/cache interaction worth
investigating; it neither proves an implementation bug nor resolves the entire
answer-quality cause. Model-capacity claims remain suspended.

## Reopened cycle 6: deployment-contract controls, queued serially

`tools/answer_plumbing_contract.py` retains **exact actual messages** for Collins
(2) and the unavailable middle name (6). Four conditions per question: native
sampling/thinking; three vendor-core sampler fields only (.1 / 50 / 1.1);
thinking enabled only; both. Common seed 42, cold requests, 2,048-token cap,
two 8K slots. No question suffix, history removal, source replacement or model
switch. Preserve the provider's separate reasoning channel and visible content.
Apply native text cleanup before evaluating final replies.

Reference: pinned model card
<https://huggingface.co/LiquidAI/LFM3-1.7B/blob/0dc4268b561499c648c7ba7c9466bf3d8d01517a/README.md>,
plus cached pinned `generation_config.json` and `chat_template.jinja`. The model
card states thinking defaults on, disabling thinking can be inconsistent, and
general core sampling is temperature .1 / top-k 50 / repetition penalty 1.1.
This does not specify every sampler detail; native top-p/min-p and llama's
penalty scope are retained and explicitly not called Transformers parity.

Artifacts will be `build/pc1/`. A successful vendor-core bundle is not proof
which of its three fields mattered; a failed bundle does not rule out shared
request-composition defects. A single seed is localization, not reliability
certification. Vendor/third-party source code remains read-only.

Cycle 6 launched in `build/pc1/` after the strict matrix exited. The full-message
sampler/thinking factor changes have passed no-model isolation tests and actual
tokenizer shape/count checks; response outcomes are pending.

### Cycle 6 outcome (`build/pc1/`)

All eight official/provider template byte and token-count checks pass, with
zero cached tokens. Cold native Collins and middle-name **exactly reproduce the
original complete raw content**, byte-for-byte. This eliminates a requirement
for original SQL timing, missing stream fragments, observer work or reported
prefix reuse to produce these particular responses; it does not clear the
shared request composition or backend arithmetic.

| Condition | Collins | Missing middle name |
|---|---|---|
| Native | Wrong age claim, 370 tokens, stop | 2,048 tokens, length, exposed speculative content |
| Vendor-core sampling only | Wrong visit/shelves answer, 447 tokens, stop | 1,320 tokens, stop; native cleanup leaves a core abstention but inaccurate supporting attributions |
| Thinking only | 2,048 reasoning tokens allowance exhausted, no final content | Same: length, no final content |
| Vendor-core plus thinking | Correct living answer with SOURCE 1 / part02 binding; 1,277 total generated tokens | False “Edward Fitzwilliam Darcy”; 1,992 total generated tokens, stop |

The two thinking-only rows report 8,578 / 8,076 reasoning **characters** and
zero final-content characters. Those are not character-to-token estimates;
2,048 is the provider's total generated token count. Vendor-both Collins has
4,938 reasoning characters and 434 content characters. It demonstrates a
deployment-policy interaction on an unchanged factual input, not a universal
fix. The absent-fact counterexample explicitly rejects promoting that bundle
as a complete remedy.

## Reopened cycle 7: distinguish answer budget from model failure

The native 2,048-token limit covers all generated tokens, not only the final
visible answer. Since the middle-name replies reached that limit, they do not
establish what the model would return after finishing its reasoning.

Predeclare four budget controls on the exact middle-name messages: native versus
vendor-both settings, each at 2,048 and 8,192 output tokens. Both caps run under
the **same one-slot 16K context**, so the pair changes only reply allowance.
Total configured context stays 16K (native had two 8K slots), but this is
deliberately a diagnostic reallocation, not a claim about the actual native run.
Record the actual launch/slot/context and both reasoning/content channels. Do not
compare its timings as a benchmark against earlier runs. Artifacts will be
`build/pb1/`. If the larger cap still loops or returns false claims, retain that
failure; if it completes correctly, qualify the budget effect separately from
any contextual setting change.

The budget phase's generation HTTP timeout is explicitly 600 seconds, versus
180 for the default 2K probes. The transport must not abort an 8K-token control
early and masquerade as the model's output cap. This diagnostic-only timeout
parameter preserves all existing callers' default and has a regression control.

### Cycle 7 outcome (`build/pb1/`)

Under the same one-slot 16K configuration, native 2,048-token allowance truncates
at exactly the original 8,328 raw characters. Raising only the allowance to
8,192 lets it stop naturally after **2,359 generated tokens**. The output includes
an orphan reasoning close; actual native cleanup reduces 9,525 raw characters
to **793 visible characters**, with a final core abstention. It still has
inaccurate citation/supporting claims and is not a complete-fidelity pass.

Vendor-both stops at 1,992 generated tokens under **both** allowances and retains
the same false Edward/Fitzwilliam answer. More budget does not fix that error.
The higher native allowance did not require all 8K generated tokens: actual
prompt 5,529 + completion 2,359 + reserve 256 = **8,144**, below the actual native
8,192-slot size. The native slot-allocation confirmation below is therefore
needed before attributing the effect to output budget rather than changed slots.

## Follow-up plumbing controls: no new prompt variants

Freeze the following bounded runs before inspecting their outcomes:

1. **`build/pr1/`, remainder phase:** same native two 8K slots and original
   middle-name messages, seed and sampling. Compare max_tokens 2,048 with the
   computed remaining allowance **2,407 = 8,192 - 5,529 - 256**. This changes only
   the output cap, uses no extra context/RAM allocation, and tests whether the
   closing reasoning boundary can be reached inside the actual native window.
2. **`build/pt1/`, thinking_budget phase:** Collins and middle-name, native
   sampler fields, thinking enabled, fixed one-slot 16K context. Compare output
   allowance 2,048 versus 8,192. Cycle 6's thinking-only truncation cannot be
   treated as a completed model failure; cycle 7's vendor-both setting already
   stopped naturally, so it does not answer this missing condition.
3. **`build/pq1/`, backend phase:** the two unchanged native requests and 2K
   allowance, same two 8K slots, now under strict CPU/BF16 KV with all GPU devices,
   operation offload and KV offload disabled. Compare complete replies with pc1's
   exact native GPU replay. The six-point arithmetic comparison is not itself an
   answer-quality comparison.

Run serially, retain all complete reasoning/content fields and native-visible
transforms, and use the 600-second generation transport allowance for these
controls. These test runtime settings and limits, not answer-hint instructions.
No user-specified reply limit is silently raised in production; 2,407 is an
explicit diagnostic counterfactual. The final citation defects remain separate
even if the core abstention improves.

### Source audit: why a blind larger-default patch is unsafe

`src/app/model_settings.c` explicitly defaults to a 2,048-token generation cap;
`chat.c` reserves against that configured cap and disables thinking in the
request builder. This is not an accidental HTTP truncation. The model can still
emit reasoning as content with thinking disabled, as the native capture shows.

The current recall reservation is all-or-none: reserve 4,096 tokens only when
the adjusted builder context exceeds 4,096 + 1,024 + 256. With an effective 8K
slot, configured cap 3,584 reserves all 4K but leaves only 256 base-content
tokens; cap 3,840 reserves **zero** and permits 4,096 base-content tokens.
Cap 4,096 likewise reserves zero. This discontinuity is source-derived in
`build/answer-plumbing-reservation-audit.json`, **not a measured native admission
failure**. Actual admitted counts still depend on the constructed request and
planner. Raising the default could change evidence/history composition, so the
frozen-message larger-budget result must not be silently promoted into an
unchanged-input production fix.

SQL ingestion explicitly excludes stopped/truncated assistant replies
(`memory_store.c:13-15`). Recent conversational history has different handling;
its retention is not proof that SQL/vector eligibility is broken. The truncated
middle-name reply is last in the original seven-question run and therefore
cannot explain the earlier income/Collins failures. Keep those causal boundaries
when investigating propagation of partial reasoning into later conversation.

## Follow-up qualification: pr1 / pt1 completed; pq1 incomplete

All six pr1/pt1 arms match official rendered template bytes and provider prompt
counts, with zero cached tokens. Actual native C cleanup was applied; separate
provider reasoning-content lengths are retained in `template-controls.json`.
Cap-only prefix comparisons are in each run's `budget-controls.json`.

- **pr1:** actual two 8K slots confirmed. Cap 2,407 completes at 2,359 tokens,
  extending the entire original 2,048-token raw reply and matching pb1's completed
  message exactly. Native cleanup yields the same 793-character visible reply.
  Consumption plus reserve is 8,144. This isolates the output-cap effect within
  the original slot configuration, not just a larger-context experiment. The
  visible abstention still contains inaccurate support and is not a fidelity pass.
- **pt1 Collins:** native sampling with thinking enabled finishes at **6,226**
  generated tokens; 25,387 reasoning characters, 555 final-content characters.
  Its final answer incorrectly claims shelves/approval rather than the living.
- **pt1 middle name:** finishes at **5,012** generated tokens; 19,189 reasoning
  characters, 759 final-content characters. Core abstention is present, but the
  reply invents “Sir Darcy” and misdescribes the supplied opening excerpt.
- Both pt1 2K arms produce no final content. Each larger-cap reasoning string
  starts with the entire corresponding 2K reasoning string. Longer reasoning
  completes generation but does not establish correct evidence-to-claim binding.

**pq1 failed on its first request, Collins.** No completion response or results
file exists; middle-name was never submitted. The server log records prompt
progress followed by steady generation, reaching **2,025 generated tokens** at
9:59.596 before cleanup at 10:01.596 and task cancellation at 10:02.065. The
nonstreaming client timed out awaiting response headers at its 600-second bound;
this is not evidence of a hung model or a completed bad CPU answer. The owned
server is terminated in the runner's finally block. Retain the failed run and
traceback, and do not assert whole-reply CPU/GPU parity or a CPU quality result.

No production defaults are promoted. The evidence supports a concrete
reasoning-boundary/output-cap failure, alongside still-unresolved factual and
citation failures. “Enable thinking and increase tokens” is falsified as a
complete remedy by completed Collins. Shared request/evidence representation
remains open even where official template bytes and token counts match.

Run cycles 6 and 7 only after the arithmetic matrix exits. Their transformation
tests verify unchanged messages, isolated sampler/flag changes, and cap-only
differences. No diagnostic setting is automatically promoted into production.

### Tokenizer API-shape defect found in diagnostic preflight

The actual Transformers tokenizer returns `BatchEncoding` from
`apply_chat_template(tokenize=True)`, with keys `input_ids` and `attention_mask`.
Its `len` is **2 fields**, not the token count (a measured Hello example has
13 tokens). This affected the preflight guards/count metadata in
`answer_live_ablation.py`, `answer_manifold_counterfact.py`, and the not-yet-run
deployment-contract probe. It does **not** affect production C counting through
llama-server, or the numerical parity/manifold probes that explicitly rendered
text then encoded it.

Preserved original scripts and invalid preflight artifact under
`build/answer-plumbing-api-contract/`. Remediated all three sites to render text
with `tokenize=False`, require a string, encode with no extra special tokens,
and require a flat token-ID list. Regression tests include a real-shape mapping
double that would make the old code count two fields. An actual cached-tokenizer
integration control confirms upcoming native prompts are **5,267 / 5,529** tokens
(thinking-enabled **5,266 / 5,528**), not two.

Existing `build/ala1/` and `build/amc1/` immutable manifests remain unchanged.
New `token-budget-addendum.json` files recompute every actual token count and
match saved provider usage; all nine actual full reply allowances fit 8K.
Their response observations stand, but their original preflight guards were
invalid. The already-recorded later template/provider count checks were valid.
This is a diagnostic plumbing fix, **not proof of the product's underlying
answer-quality cause**. The new contract and budget models have not run yet.

Arithmetic scope clarification: ap2's appended token prefixes are the frozen
**unpenalized greedy diagnostic** continuation from alp1, not the original native
temperature-.7 sampled continuation. In fact the original native income reply
has only 237 generated tokens, so candidate position 259 is not even on that
original trajectory. These controls expose a runtime parity problem on exact
live-input-derived prefixes; they do not establish that the .951 discrepancy
caused the native ten-thousand answer. The queued full native strict-CPU replay
is needed for the next end-to-end backend comparison.

Production transport audit: `chat.c` passes 120,000 ms to the streaming HTTP
client, but `http.c:353-375` refreshes the receive deadline as bytes arrive.
That is an inactivity bound, not a two-minute total-generation cutoff. Thus no
production timeout change is justified merely because a long healthy stream
can exceed two minutes. The diagnostic nonstreaming 600-second allowance is a
separate requirement because its client waits for the complete response.

### Original native stream metadata audit

The preserved live SSE also contains provider timing metadata. Exported it to
`build/answer-live-latest/provider-cache-metadata.json` rather than assuming
`cache_prompt=true` meant a reused prefix. Opening, income, Collins, Georgiana,
camp and middle-name all report **cache_n=0**; only the age question reuses 84
tokens. In every case, `cache_n + prompt_n` equals the independently rendered
prompt count (5,408 / 5,796 / 5,267 / 5,668 / 5,519 / 5,431 / 5,529).

Thus the original primary failures do not require reported prompt-prefix reuse,
and this metadata provides no evidence of hidden input-token clipping. This is
not a proof that every recurrent-state reset or slot operation is correct. The
native middle-name stream's 2,048 generated tokens remain a concrete output-cap
boundary for cycle 7.
