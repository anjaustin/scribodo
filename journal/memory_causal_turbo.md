# Causal investigation — adversarial checks and preregistration

## Cycle 1: attack the shared-engine conclusion

Claim attacked: raw completion plus CPU/Metal agreement establishes a learned
model limitation. Counterexample: all three can share a wrong template or an
implementation error. The claim does not survive as stated.

Refined dependency model:

`originals -> assembled messages -> official/llama template -> token IDs`

`token IDs -> reference/llama inference -> continuation -> visible answer`

At each split, compare the same inputs. Existing independent runtime checks on
other prompts are supporting evidence, not a substitute for this comparison.

## Cycle 2: attack the “memory failure” diagnosis

A perfect source packet already fails some short authority/task questions.
Adding storage cannot logically guarantee the missing interpretation. However,
classifying the episode correctly also does not guarantee that an open-ended
assistant will preserve it while drafting. These are separate diagnostic tasks.

Refined fault partition:

1. Input/template/token identity defect.
2. Runtime computation/state defect under matched tokens.
3. Evidence/speaker/state discrimination failure under minimal instructions.
4. Correct discrimination but failed task/authority preservation in composition.
5. Citation/output-contract failure independent of semantic state.

## Execution rules

- No model inference overlaps the active post-admission native replay.
- Use cached pinned official weights/code; no author documents sent remotely.
- Keep scripts and original runtime artifacts immutable; new experiments get
  owned private run directories and source/config hashes.
- First select a passing fact control and actual failing native cases; compare
  official template/token IDs before independent generation.
- Match greedy sampling/repetition control and record the full requested budget,
  not just visible output. Token differences are a result, not permission to
  call inputs matched.
- Evaluate semantic parity separately from exact token agreement. If greedy
  outputs diverge, use teacher-forced shared prefixes and next-token ranks/gaps
  before declaring a runtime bug.
- Only then choose a small finite interpretation/composition intervention set.
  Do not run an unbounded prompt search or turn DEV outcomes into qualification.

## Frozen first parity experiment

Prepared run: `build/mc-l7ens7nd`. The earlier `mc-m54my2ew` was preparation only;
the driver was hardened for immutable script/helper identity before inference,
then a new run was frozen. Neither preparation loaded model weights.

`tools/memory_causal_parity.py` selects original native answer indices 1, 4 and
26: a passing count control, repeated-user count failure, and alternating-history
deliberate-ambiguity failure. Native messages and 1,024-token allowance are
unchanged. Official template/token preparation succeeded using the pinned cached
tokenizer. The separate native admission replay has completed; llama/reference
inference is now running sequentially, with no overlapping model processes.

The initial launch attempted to execute the archived driver from its evidence
directory, which made its repository-relative binary path resolve under
`build/build/`. It failed before starting a server or generating any answer
(`build/memory-causal-parity.log`). Relaunching the hash-identical original tool
from `tools/` fixes only that invocation path; frozen cases and code are unchanged
(`build/memory-causal-parity-v2.log`).

The diagnostic environment was rebuilt entirely from the offline package cache:
PyTorch 2.15.0 and Transformers 5.22.1. The pinned cached reference uses F32 CPU
compute from official BF16 weights, matching the earlier runtime-fidelity study;
no automatic kernel download, remote model or author content is involved.

An important matching detail: llama.cpp's repetition penalty scans the last 64
tokens, whereas a default Transformers repetition penalty scans the entire input.
The reference therefore uses an explicit last-64-token processor at penalty 1.05.
Source check: `common/common.h` sets `penalty_last_n=64`; server
`init_sampler()` accepts the complete prompt before generation. A superficially
similar `generate(repetition_penalty=1.05)` would not be a matched experiment.

The driver refuses template/token mismatch, changed scripts/helpers/artifacts,
and overwriting earlier stage results. It records raw top-token distributions at
the start and at the first divergent shared prefix. Full answer semantics and
near-tie probability differences must be reviewed separately.

## Cycle 3: independent implementation results

`build/mc-l7ens7nd` completed all stages. All three official/llama template byte
strings and token arrays matched. The count control matched for 34 generated
tokens. The repeated-history case matched for 157 tokens before diverging;
both answers abandoned the count question for weather drafting. The reference
truncated at 1,024 tokens. The ambiguity case diverged after four tokens but both
answers invented draft details and/or a future resolution of protected ambiguity.

At position zero and first shared-prefix divergence, all reference top-10 tokens
were present in llama's top-64. Maximum raw log-probability gaps ranged from
0.035 to 0.083. This does not establish bit-exact runtime equality or exclude all
runtime defects; it does exclude a llama-only defect as a necessary cause of
these two semantic failure families. Both independently reproduce them.

The raw ambiguity responses also explicitly restate the correct source constraint
before contradicting it in proposed prose. This damages the hypothesis that the
only missing capability is extracting the author's stated constraint. It is not
proof of correct internal understanding; it is observable interpretation/output
inconsistency. Raw model output contains unexpected closing thought delimiters
despite the disabled-thinking prefix; visible/native postprocessing and full
generation costs must remain separate.

## Frozen next contrast: interpretation versus response composition

Before inference, fix 24 cases generated by `tools/memory_causal_cases.py`:

- Four minimal synthetic author states: proposed/chosen cloak color and
  protected/pending ambiguity. Counterfactual pairs change only the final author
  statement, not the preceding proposal or available evidence.
- Each state gets neutral versus unchanged production creative persona, and
  open recall versus a short status-classification question: 16 cases.
- Repeat protected/pending × persona × task on the actual native ambiguity
  request, retaining its source wrapper and alternating recent history: 8 cases.
- All arms have 512 reply tokens, temperature zero, repetition penalty 1.05,
  disabled thinking, no prompt cache, same local pinned runtime. The native
  diagnostic subset therefore differs from the original 1,024-token/cache-enabled
  run; do not present it as a matched replacement quality result.
- No grammar, classifier implementation, semantic ledger or production prompt
  change. Status-format compliance and substantive interpretation are scored
  separately by reading complete responses. Draft invention is not mistaken for
  recalled fact when genuinely requested; these questions ask for recall/status.

Predictions: failure even on minimal classification places the boundary before
free-form composition. Classification success paired with recall failure places
a boundary in task/authority preservation during response generation. Persona
contrasts test a writing-prior contribution, not the full underlying training
mechanism. Passing this toy set would not qualify memory or a classifier.

## Cycle 4: the classification shortcut fails

The 24-case run completed at `build/mb-z034xk06`. All requests retained their
current questions. The minimal creative chosen-color recall correctly states
that the author chose saffron; the corresponding short classification returns
PROPOSED. Protected ambiguity classifies as PENDING under both personas; pending
ambiguity classifies as PROTECTED under the creative persona. This occurs on
94–181-token minimal prompts, so long history is not required.

The actual native protected-ambiguity open recall becomes faithful under the
neutral persona, while creative recall repeats the invented causal prose. That
is a within-input persona intervention, not proof that a neutral persona solves
all tasks: neutral protected classification is still wrong, and native creative
classification expounds until the 512-token cap despite the requested one label.

This damages the proposed neat split between classification/recognition and
composition. A classification question also introduces label binding, response
format, and wording effects; its error cannot be equated with inaccessible
latent state. Both substantive interpretation and protocol compliance must stay
separate. A typed ledger/classifier would not be justified by these observations.

## Fixed label-binding countertests

Before running them, fix 32 minimal conditions in
`tools/memory_causal_label_cases.py`: four states × two personas × four question
forms. Keep the exact same chronological evidence as the first contrast.

1. Reverse only the requested label order relative to the first contrast.
2. Define both labels explicitly before asking the original classification.
3. Ask the underlying commitment/obligation as a direct YES/NO question.
4. Ask the same underlying distinction in one natural-language sentence.

All sampling/context settings remain those of the 24-case contrast. These tests
can separate label/order/wording sensitivity from an inability to answer even a
direct state question. They are a finite diagnostic follow-up to a damaged
hypothesis, not prompt optimization for a new quality gate or a production fix.

## Cycle 5: explicit definitions are not a universal cure

The 32 cases completed at `build/mb-2hlvobbp`, without truncation. Underlying
state judgments: reversed labels 5/8 correct, defined labels 5/8, direct boolean
7/8, direct one-sentence statements 8/8. The original-label minimal contrast was
4/8. These are narrow compound-free DEV diagnostics, not product accuracy.

Definitions damage the proposed-color answers: both personas return CHOSEN even
though the author explicitly has not chosen. Reversing label order changes the
creative color answers from always PROPOSED to always CHOSEN, trading which
counterfactual is wrong. This breaks “the model simply cannot distinguish the
underlying states” as a sufficient account: it can state the distinction in
natural language, while label mapping changes the reported decision.

Conversely, direct boolean is not perfectly reliable either: creative pending
ambiguity answers NO to whether resolution is required before continuing, while
its one-sentence answer says it must first be settled. No output form earns a
general reliability claim from this toy set.

Before any remedy is proposed, rerun the exact same 32 cases at repetition
penalty 1.0, keeping other settings fixed. A repetition penalty could bias short
label outputs; earlier penalty controls addressed different lookup prompts and
cannot rule that out for this new contrast.

## Cycle 6: sampling control and stop

`build/mb-83f16k34` repeats the same 32 cases at penalty 1.0. All 24 categorical
and boolean outputs are byte-identical to the 1.05 run. All eight natural-language
outputs still express the correct underlying author state, although one now uses
two sentences and misattributes part of the assistant's suggestion. All finish.
Therefore neither the short label errors nor the direct-boolean counterexample
require a repetition penalty.

The simpler surviving model is not a new hierarchy: author-state meaning is not
reliably conserved through this model's changing task/output contracts. Longer
history and the writing persona add failure modes, but are not necessary for
the short label failures. Exact engine/template comparisons and narrow causal
contrasts support that engineering boundary; they do not identify a unique
neural/training-level origin.

Stop this pass rather than optimize wording to make the DEV set green. Record
the supported remedy boundary: author/host-controlled evidence and authority,
explicit separation of recall and drafting, and independent model/workflow
qualification before claiming automatic continuity. No classifier, ledger,
additional tier or default-on behavior is implemented by this investigation.
