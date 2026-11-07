# Memory quality: controlled root-cause investigation

**2026-11-07. Diagnostic evidence, not a new quality-gate result.**

The frozen V2 result remains FAIL. This investigation separates retrieval,
request composition, factual answering, citation/schema compliance, and brief
generation. No production settings, prompts, schemas, frozen fixtures or scoring
were changed. `tools/memory_bedrock_probe.py` uses an owned local server, new toy
facts, pinned production weights and production launch flags, with explicitly
labelled diagnostic overrides. No SQL, Qdrant, remote provider or author server
is used. Failed-brief replay is restricted to the already-used **DEV** dataset.

## 1. The checkpoint can answer; composed requests can destroy that ability

Fresh fact: “Mara Bell keeps 47 copper keys.” Question: “How many copper keys does
Mara Bell keep?” Temperature zero; no prompt caching; the same pinned LFM3 BF16
model and llama.cpp binary as V2.

| Controlled condition | Observation |
|---|---|
| Single plain question | Answers 47 in two completion tokens |
| Single strict-JSON request with production source framing | Answers 47, but copies an entire source header as its citation ID |
| One irrelevant prior user turn + strict protocol | Answers 47 |
| Two irrelevant prior user turns + strict protocol | Abstains despite the correct source and question being present |
| Two very short prior turns, total 243 prompt tokens | Still abstains; crossing 256 tokens is **not necessary** |
| Single request with a full SHA-256 source ID, 263 prompt tokens | Answers 47; crossing 256 tokens is **not sufficient** |
| Same two-turn failure at batch 64, 256 and 512 | Same abstention |
| Same failure with repetition penalty 1.0 rather than inherited 1.1 | Same abstention |
| Same rendered token IDs via raw `/completion` | Same output as chat completion |
| Same three controls on CPU instead of Metal | Same outputs |
| Same two-turn history but plain source labeling | Correct answer and copied short ID |
| Same two-turn history with shorter instructions | Correct factual answer; formatting remains sensitive |
| Same 64-turn history, plain fact/question and no strict evidence protocol | Correct factual answer: 47 |
| Same 64-turn history with strict JSON/source protocol | Discusses irrelevant weather history or emits invalid output |
| Production creative-writing persona + native source framing, zero/two prior user turns | Answers 47 |
| Same production persona/framing, 64 repeated prior user turns | Ignores the fact/question and discusses the repeated weather text |
| Same production persona/framing, 64 alternating user/assistant turns | Answers 47 correctly |
| Same production persona/framing, history combined into one message | Answers 47 correctly |

All examined template renderings contain the supplied fact and final question.
A repeated minimal lookup after the history tests again answers 47. Thus missing
retrieval data, response-parser corruption, persistent slot corruption, a simple
batch-size bug, the repetition penalty, and a hard 256-token cliff do **not**
explain these observations by themselves. CPU agreement excludes a Metal-only
explanation, not every possible shared llama.cpp defect. A fresh official
Transformers comparison on these exact prompts has not been performed.

**Supported diagnosis:** the model/protocol interaction is brittle. Role layout,
long policy/format instructions and source framing jointly change factual
behavior. A correct source in the request does not establish usable recall.
Conversely, V2's 5% compound score is not a measurement that the UI recalls only
5% of facts: the harness adds strict JSON/citation requirements and does not run
native ChatSession/UI generation.
The production-persona control also fails under the repeated 64-user-turn history,
so the defect is not merely an artificial JSON benchmark requirement. These
controls use the persona/source framing, not the complete native ChatSession/UI
path. The alternating-history toy control succeeds; it does not establish broad
accuracy on ordinary writing sessions.

**Evaluation defect exposed:** V2 constructs the common recent tail as 64
identical, consecutive user messages, rather than ordinary alternating history.
That remains a useful adversarial robustness case, but should not be the only
default history used to estimate writing-session quality. Keep its negative
result and retain the case in future tests; add representative native histories
and report ordinary versus adversarial performance separately. The investigation
does not justify deleting a difficult case or declaring a new pass.

## 2. Citation identifiers are a separate, avoidable burden

Even a correct “47” answer can copy the whole header as `id`, return `SOURCE 1`
instead of its opaque unit ID, invent `source-one-message-1`, or return a number
where the protocol requires a string. Simplifying the source presentation can
restore exact short-ID citation on the toy case. That is diagnostic evidence,
not permission to relax validation of the frozen experiment.

V2 asks for “original IDs,” while the renderer exposes a numbered SOURCE,
`unit=...`, `message=...`, role and byte range; its scorer expects the unit ID.
The binding is needlessly implicit. A future protocol should offer explicit,
short per-request citation handles and bind them to immutable SQL locators in
host code. The model should choose a handle and supporting quotation, not
reconstruct a provenance hash. Host-side quote/source validation remains strict.

## 3. The first T1 failure is a reproducible compression-contract failure

Replaying the first four **DEV** inputs with the unchanged production brief
instructions/schema reproduces the original 725-token prompt and 256-token
truncated completion. The model enumerates eight weather observations, mostly
copying individual sentences from the first source. It does not produce a
compact, deduplicated summary.

| Same DEV input | Outcome |
|---|---|
| Production grammar, 256-token allowance | Incomplete JSON; finish reason `length` |
| Grammar removed, still 256 tokens | Same truncated enumeration |
| Diagnostic allowance raised to 512 | Finishes in 277 completion tokens; still exceeds the **unchanged production** 256-token budget |
| First source alone, 256 tokens | Finishes in 217 tokens, enumerating six weather observations rather than compressing them |

The schema permits up to eight facts with 256 characters each; it does not ensure
the total serialized output fits 256 tokens or the separate 1,536-byte host cap.
The prose instruction states those aggregate limits, but the model does not
reliably allocate output to obey them. The host correctly rejects truncation.
Removing the grammar does not fix it; blindly increasing the allowance would
violate the intended resource contract rather than establish useful summaries.

This is content-dependent, not universal schema incompatibility: the same
production prompt/schema successfully summarizes one short toy fact in 35
completion tokens, four facts in 94, and four facts with repetitive narrative
filler in 90. The first sequential DEV batch instead elicits verbose extraction.
Because publication requires a contiguous frontier, that failed first batch
blocks later briefs. Bounded retries protect resources but do not provide useful
continuity when the compression contract repeatedly fails.

## What to change next — separately preregistered, not tuned to V2

1. Establish a native-persona factual baseline separately from structured evidence
   compliance. Test ordinary and adversarial histories; preserve abstention and
   contradiction tasks. Do not treat one toy success as a whole-manuscript pass.
2. Make source/handle semantics explicit and compact. Keep authoritative mapping,
   originals and provenance in host code. Measure factual fidelity and citation
   fidelity separately, then require both at admission/release gates.
3. Define T1's actual compression objective: deduplicate predictable narrative,
   prioritize persistent entities/constraints/decisions, preserve uncertainty,
   and allocate a response structure that can realistically close within its
   fixed token budget. Validate it on diverse DEV sources before any held-out run.
4. Only after those controls pass, register a fresh evaluation with frozen unseen
   cases, native rendering, matched budgets and honest coverage/cost reporting.
   Keep V2's failed reports and thresholds intact. No T2 or default-on change.

## Evidence

Diagnostic logs and raw request/rendering/response JSONL are retained locally:

- `build/memory-bedrock-probe.log`: initial composition ablations.
- `build/memory-bedrock-{production,penalty1,batch64,batch512}.log`.
- `build/memory-bedrock-{raw-controls,cpu-controls,prompt-controls}.log`.
- `build/memory-bedrock-brief-controls.log`: production brief literals, toy inputs.
- `build/memory-bedrock-brief-dev-replay.log`: exact DEV-input replay.
- `build/memory-bedrock-final-controls.log`: corrected byte metadata, short-tail,
  full-ID and 64-turn plain-answer counterexamples.
- `build/memory-bedrock-persona-controls.log`: production persona with source
  framing, without the benchmark's JSON instructions.
- `build/memory-bedrock-history-layout.log`: alternating versus combined history
  with the production persona and source framing.

Each log identifies its private `build/mb-*` directory with manifest, props,
server log and complete request/rendering/response events. Later runs archive
the diagnostic script too. Initial toy source headers mistakenly said 29 bytes
instead of 31; the final controls correct that metadata and reproduce the key
failure and recovery, so that typo does not explain the result.

Follow-up: the complete native DEV comparison and explicit no-go decision are
recorded in [MEMORY-011](MEMORY-011-native-continuity-dev.md). It does not replace
or rescore the frozen V2 failure or these diagnostic controls.
