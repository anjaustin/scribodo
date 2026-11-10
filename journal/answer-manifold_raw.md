# Answer reliability manifold — RAW evidence brief

2026-11-09. Full LMM pass, using the repository's four phases plus the installed
method's adversarial verification loop. Method source:
<https://github.com/anjaustin/lmm>. This is an evidence/uncertainty brief, not a
claim that the answer or a new architecture is already known.

The product must answer the author's current question faithfully while preserving
author authority, consent, source eligibility and ordinary conversational use.
The SQL store, vector index and model must cooperate; success in one component
does not imply end-to-end success. Existing diagnostics preserve actual requests,
provider streams and complete replies. Their measurements constrain the next
experiments but do not establish reliable generalization.

The available evidence includes intact exported SQL source bytes, correctly bound
retrieval hits, a discovered income passage lost after fusion, and admitted
opening/Collins passages followed by incorrect answers. Some entity/source errors
occur in independent-reference shared greedy prefixes. Nevertheless numerical
runtime discrepancies exceed the existing tolerance. The runtime cannot yet be
declared equivalent. Removal experiments change both content and token positions;
they establish sensitivity, not a uniquely identified interference mechanism.

Important uncertainties remain. Does the native repetition penalty alter the
answer under an otherwise identical chat endpoint and complete reply cap? Does
prior-history content matter after matching each message's token count, or does
removing history primarily change position and length? Can an explicit, narrow
answer contract improve source-bound factual output with the full context left
intact? Does such an instruction merely suppress visible elaboration while leaving
the underlying answer wrong? Can the model read one real passage without inventing
definitions if no definition was requested? Is fixing selection necessary on some
questions but irrelevant on others? What happens when the author corrects a source,
when sources disagree, or when the requested fact is absent?

Potential changes have costs: extra retrieval can introduce distractors; deleting
history can erase author intent; an extractive-only interface can damage creative
collaboration; automatic answer validation can share the generator's mistakes;
increasing context can exceed local memory; a model replacement can hide pipeline
defects. No estimated readiness percentage resolves these tradeoffs. The next pass
must distinguish these alternatives with small, falsifiable interventions and
report failures as prominently as successes.

## User steering: reopen plumbing before capacity claims

The user emphasizes the model's capability and suspects the integration. This
is compatible with the measured narrow source-following successes. In particular,
shared official-reference errors do **not** exclude application-level plumbing:
the reference consumed the same application-composed messages. They only exclude
an explanation unique to one inference backend for those shared prefixes.

The pinned model card documents thinking enabled by default and warns that
disabling thinking can be inconsistent. Its general sampling recommendation is
temperature 0.1, top-k 50, repetition penalty 1.1. The captured product request
uses thinking disabled, temperature .7, top-k 40, repetition penalty 1.05.
These are observed differences, not proof of the cause. Reopen deployment
contract and numerical backend/cache controls before interpreting failures as a
model-capacity limit. No vendor code or production policy is changed by this audit.
