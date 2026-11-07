# Causal continuity investigation

**2026-11-07. Diagnostic work, not a replacement quality gate.**

## Question

Which transformation first fails: evidence admission, message/template/token
construction, inference computation, interpretation of author state, or execution
of the requested answer? “Model/protocol brittleness” describes observations but
does not distinguish these causes.

The [native DEV no-go](MEMORY-011-native-continuity-dev.md) remains unchanged.
No new memory tier, generated aid, classifier, model switch or production prompt
change is justified by this investigation alone.

## Independent-runtime test

The earlier raw-completion and CPU/Metal controls all used llama.cpp. They could
not exclude a defect shared by that engine/template/tokenizer. Earlier official
runtime parity tests used other inputs, not these failing continuity cases.

`tools/memory_causal_parity.py` freezes three actual native DEV requests from
`build/mn-z0h05o1h`: index 1 (passing count control), 4 (repeated-history count
failure) and 26 (alternating-history ambiguity failure). Full native messages and
1,024-token allowances are retained. The independent reference is the cached,
pinned LiquidAI implementation under Transformers 5.22.1/PyTorch 2.15.0, CPU F32
compute widened from official BF16 weights, as in MODEL-001. The environment was
recreated entirely from offline packages. All inference remained local.

Both stages use greedy decoding, repetition penalty 1.05 on the last 64 tokens,
and no prompt reuse. Default Transformers repetition penalties cover the whole
input, so an explicit last-64-token processor is required to match llama.cpp.
These are no-cache/raw-completion diagnostics, not an exact recreation of native
streaming/cache state. Reference and llama run sequentially, not simultaneously.

Results retained in `build/mc-l7ens7nd`:

| Native input | Official/llama template and token IDs | Shared greedy prefix | Full-response outcome |
|---|---|---:|---|
| Passing count | Exact match | 34 tokens (entire continuation) | Both answer 58 |
| Repeated-user count | Exact match | 157 tokens | Both abandon the count question and draft weather prose; reference truncates at 1,024 |
| Protected ambiguity | Exact match | 4 tokens | Both invent draft details and/or a future resolution rather than faithfully report protected ambiguity |

At the initial position and the first divergent shared prefix, every reference
top-10 token appeared in llama's top-64. Maximum raw log-probability gaps were
0.035–0.083. These are observations, not a new correctness threshold. Distributions
are before repetition penalties; raw top-token margins must not be interpreted
as the final penalized greedy margin. Greedy divergence alone is not a bug.

**Supported exclusion:** a llama.cpp-only defect is not necessary for these two
semantic failure families. The official independent implementation also exhibits
them on the same token input. This does not exclude every runtime defect, prove
all configurations equivalent, or isolate a neural/training-level cause.

## Recognition is not faithful execution

In the ambiguity case, both raw continuations explicitly restate the author's
constraint correctly, then generate prose with unsupported fire mechanisms or a
plan to settle the protected uncertainty later. This is observable inconsistency
within a response. It damages the hypothesis that simply extracting the correct
constraint into another memory record is sufficient.

It does **not** prove correct internal understanding: repeating a constraint is
not the same as representing and applying it. A narrower contrast is required
between state discrimination and preserving that state while answering.

Raw outputs also include unexpected closing thought delimiters despite the
official disabled-thinking generation prefix. That behavior is present in the
independent reference too. Full generation costs and native visible answers must
be kept separate; parser behavior cannot explain away unsupported prose that
is present in the raw continuations themselves.

## Next bounded contrast

`tools/memory_causal_cases.py` fixes 24 DEV conditions before inference:

1. Four synthetic state cases: proposed/chosen cloak color and protected/pending
   ambiguity. Counterfactual pairs change only the final author statement.
2. Neutral versus unchanged production creative persona.
3. Open recall versus a short status-classification question.
4. Repeat the ambiguity contrasts on the actual native source wrapper and recent
   alternating history, not only on a simplified transcript.

All 24 conditions use 512 reply tokens, temperature zero, repetition penalty
1.05, disabled thinking and no prompt cache. No grammar or production change.
The native subset differs from the original 1,024-token/cache-enabled run and
must not replace it as quality evidence. Status-format compliance and substantive
state fidelity will be reviewed separately using complete answers.

Passing classification but failing open recall would locate a task/composition
boundary, but only if the classification contract itself is reliable. The actual
results broke that assumption; a classification question also requires mapping
meaning into labels and obeying an output format. A wrong label alone cannot
identify an earlier internal discrimination failure.

## Task and label contrasts: observations

The 24 cases completed at `build/mb-z034xk06`. Only native creative protected
classification truncated; all other responses finished. The following are
within-case contrasts, not population accuracy estimates:

- With the same short accepted-color evidence and creative persona, open recall
  correctly says the author chose saffron; classification says PROPOSED.
- Of eight minimal original-label classifications, four are substantively right
  and four wrong. These prompts are only 104–181 tokens; long history is not
  necessary for wrong status labels.
- Protected ambiguity is labeled PENDING under both personas. Pending ambiguity
  is labeled PROTECTED under the creative persona.
- On the actual native ambiguity input, replacing the creative system prompt
  with a neutral one produces faithful open recall. It does not fix protected
  classification. The system-prompt intervention also changes prompt length;
  it is not an isolated neural explanation about creative training.

A second, preregistered 32-case follow-up (`build/mb-2hlvobbp`) holds the four
minimal evidence states fixed and varies only the question form/persona. Every
response finishes:

| Question form | Substantively correct / 8 |
|---|---:|
| Original requested labels, from the first contrast | 4 |
| Reverse only the label order | 5 |
| Define both labels explicitly | 5 |
| Direct YES/NO commitment/obligation question | 7 |
| Direct one-sentence natural-language statement | 8 |

The one-sentence judgment concerns the underlying author-state distinction, not
all dimensions of native task quality. These eight small DEV sentences are not
a new quality gate or a classifier qualification.

The strongest counterexample to a simple “more clarity fixes labels” claim is
the proposed-color case: explicit definitions make both personas answer CHOSEN
despite the author explicitly saying no color was selected. For the creative
color cases, reversing label order flips both answers from PROPOSED to CHOSEN,
fixing the accepted case while breaking the unaccepted one. There is no new
evidence about the author between those answers.

**Narrow causal conclusion:** the same supplied evidence supports a correct
ordinary statement but a wrong semantic label under an equivalent task. Output
contract and prompt presentation are part of the failure, not just storage,
retrieval, long history or an inability to repeat the author's words. This is
observable failure of meaning preservation across answer forms; it is not a
claim about a uniquely identified neural circuit or training example.

One further fixed control reruns the same 32 label cases with repetition penalty
1.0 instead of 1.05. This checks whether the diagnostic sampling intervention is
causing the label errors. No new cases, expected answers, templates or production
settings are changed.

The control completed at `build/mb-83f16k34`: all 24 categorical/boolean outputs
are byte-identical to the 1.05 run. All eight sentence answers still express the
correct underlying state, though prose varies and one now uses two sentences
and misattributes part of the assistant's suggestion. This is not a full format/
role pass. No response truncated. The repetition penalty is therefore not
necessary for these label and direct-boolean errors.

## Engineering conclusion and remedy boundary

At the observable engineering level, the failure is **unstable binding between
evidence, author-state meaning and the requested answer operation**. The model
does not reliably preserve the same decision across equivalent answer forms;
the creative-writing system prompt can also steer recall into invention. It
would be incorrect to describe all failures as missing facts, inadequate memory
capacity, or an inability to read the relevant sentence. It would also be
incorrect to infer a uniquely located neural/training defect from these tests.

This rules out simply relocating the responsibility into a generated facts list,
typed summary or decision classifier: that layer would perform another version
of the already-failing semantic transformation and could make the error durable.

The smallest defensible design direction is to separate **reporting evidence**
from **creating prose**, and keep author authority outside an unqualified model:

- Show exact original evidence and host-bound provenance without requiring the
  model to reconstruct source identity or declare a choice canonical.
- Test an explicit author-selected recall/status operation separately from
  creative drafting. Do not rely on an unqualified automatic intent classifier
  to choose the operation; a neutral prompt alone is not a universal fix.
- If persistent decisions are introduced, their authoritative status must come
  from explicit author action or separately qualified validation, not merely a
  generated label. This is a possible future workflow, not a ledger implemented
  by this investigation.
- Generated interpretations and drafts remain proposals. Host code can verify
  IDs, exact quotations, scope and bounds; it cannot generally prove the semantic
  fidelity of arbitrary prose. That still requires author review or a model/
  workflow that earns the native task gates.

No such remedy has been qualified here. If fully automatic author-intent
continuity remains mandatory under the existing local model/resource constraints,
the tested configuration is still a no-go. A stronger separately measured model
or a narrower author-controlled interaction is a legitimate product decision;
more storage tiers are not a demonstrated substitute.

## Evidence and stopping point

The investigation completed 94 diagnostic generations: three requests under two
implementations, 24 task/persona conditions, 32 label-binding conditions, and the
same 32 at repetition penalty 1.0. This is a finite causal investigation, not a
search until the quality fixture turns green. No production behavior changed.
`journal/memory_causal_review.json` preserves complete condition-level judgments,
source hashes, protocol/role caveats and the independent-runtime comparison.

Open: neural/training-level origin, generalization beyond these tasks, reliable
operation-specific assistance, alternate-model feasibility, and a fresh held-out
native gate after any separately authorized remedy. Existing negative V2/native
results, memory default-off, T1/T2 blocks and the author's sample edit are intact.

Author-authorized follow-up: [explicit Recall/Draft DEV comparison](MEMORY-013-explicit-operations-dev.md).
Its instruction-only specialization produced mixed gains but failed the frozen
screen; no production operation routing was introduced.

Investigation records: [synthesis](../journal/memory_causal_synth.md),
[test cycles](../journal/memory_causal_turbo.md),
[evidence inventory](../journal/memory_causal_raw.md).
