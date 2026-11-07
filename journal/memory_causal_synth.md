# Causal investigation — executable next steps

Status: completed finite investigation; engineering cause narrowed, no product
remedy qualified. See [evidence report](../docs/MEMORY-012-causal-continuity.md)
and [condition-level review](memory_causal_review.json) first.

## Core finding

The tested model does not reliably conserve author-state meaning across task
and answer forms, even with the same short evidence supplied. This is not simply
a storage/retrieval problem, and generating a typed decision record does not
remove the failing transformation.

## Verified contrasts

1. The post-admission native replay completed all 42 answers with valid Sources;
   compact/episode requests and answers match the previous run exactly. The code
   repair preserves admission, not semantic qualification.
2. Independent official-reference inference reproduces the repeated-history
   wrong-task failure and the ambiguity/unsupported-drafting failure. Official
   template bytes and token IDs match exactly. A llama-only defect is not
   necessary; not every possible runtime defect is excluded.
3. In a short chosen-color episode, creative-persona open recall correctly states
   the author's choice, while status classification returns PROPOSED. Classification
   is not a direct reading of latent comprehension.
4. Reversing label order flips the creative color answers without new evidence.
   Explicit definitions fix protected ambiguity but break proposed-color cases.
   Direct natural-language statements preserve the underlying state in all eight
   toy conditions, while even direct YES/NO fails one. These are diagnostic
   contrasts, not a qualified output contract.
5. Repeating the 32 binding cases without repetition penalty leaves all 24
   categorical/boolean answers unchanged. The penalty is not necessary for those
   errors. Underlying state in sentence answers remains correct, but one answer
   breaks sentence-count and speaker-attribution requirements.

## Smallest defensible next design experiment

Separate an **explicit author-selected recall/status operation** from creative
drafting, retaining original evidence and host-bound provenance. Do not route
automatically with an unqualified intent classifier. Test the operation on frozen
DEV cases, with both correct and misleading source suggestions, and independently
qualify it before fresh held-out acceptance. A neutral persona alone is already
known not to fix every state-label question.

If persistent author decisions are needed, authoritative status must come from
explicit author action or separately qualified validation, not an unchecked model
label. Any such author-controlled workflow requires a product decision; this pass
does not implement a semantic ledger or promise that a prompt can enforce all
constraints in arbitrary generated prose. A stronger local model is another
separately measured alternative, not a silent model switch.

## Stop and remaining uncertainty

Stop after 94 additional diagnostic generations rather than tuning examples to
green. No production prompt/model/tier/default changed. The original no-go,
V2 failures, local-only/privacy boundaries, T1/T2 blocks and sample edit remain.

The neural/training-level origin is not uniquely identified. General reliability,
real author workflows, sustained use and a feasible remedy within the fixed
local-resource envelope remain unproven. If fully automatic intent continuity is
non-negotiable, the current configuration still does not meet the requirement.

References: [evidence inventory](memory_causal_raw.md),
[nodes](memory_causal_nodes.md), [testable model](memory_causal_reflect.md),
[adversarial checks](memory_causal_turbo.md).
