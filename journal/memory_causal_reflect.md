# Causal investigation — testable model

## Core statement

Locate the first failing transformation between original evidence, tokenized
input, interpretation and answer execution before prescribing a memory remedy.

## Explicit distinctions

The existing observations establish a product no-go but do not uniquely identify
its cause. Correct source admission is necessary for fidelity; it is not evidence
that either runtime computation or model interpretation is correct. Conversely,
incorrect prose does not establish that the model failed to distinguish the
underlying state: prose generation may introduce the error after interpretation.

The independent-runtime comparison resolves the first tension: byte/token parity
and predictive-distribution agreement on the actual failing cases constrain the
implementation hypothesis. Semantic agreement matters more than exact greedy
string equality; near-tied logits can produce different valid continuations.

The task-isolation contrast resolves the second tension: simple classification
can diagnose latent state discrimination without being promoted to a product
solution. If it fails too, the problem precedes free-form drafting. If it passes
but free-form response fails, evaluate task/authority preservation during answer
composition, not another retrieval or summary layer.

## Assumptions surfaced

- “The same prompt” must mean identical token IDs, not just similar readable text.
- “Runtime matched before” does not prove matching on this distribution.
- “Thinking disabled” is an intervention on the official generation prefix; it
  may matter and is not proof of missing evidence. A thinking-mode control is
  diagnostic only and must be charged for hidden tokens, time and local bounds.
- There need not be one universal failure or one universal repair.

No neural-level or training-data explanation is claimed without direct evidence.
