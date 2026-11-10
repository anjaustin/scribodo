# Answer reliability manifold — NODES

## Observations and uncertainties

1. **Integrity is not relevance.** 820 source units pass exported integrity
   checks; this does not establish discovery or admission for any question.
2. **Discovery is not admission.** The income original is vector rank 4,
   reconstructed fused rank 9, outside the eight-unit admission cap.
3. **Admission is not use.** Collins' exact original is first after fusion and
   arrives intact, yet the native answer is wrong.
4. **Core fact is not complete fidelity.** Correct answer words coexist with
   fabricated quotations, wrong entities, wrong source labels and definitions.
5. **History-removal confound.** Removing 267 tokens changes both semantic
   content and positions. Prior false income claims are plausible interference,
   not yet an isolated cause.
6. **Decoding confound.** The unpenalized numerical diagnostic and native-penalty
   ablation use different request routes/samplers/caps. Their different Collins
   answers cannot yet be attributed solely to repetition penalty.
7. **Task contract is under-tested.** The persona is a creative writing partner;
   no measured full-context comparison isolates a narrow factual-answer contract.
8. **Independent semantic failure is bounded.** Shared reference prefixes contain
   errors, but the sampled native answer has not independently been reproduced.
9. **Numerical gap remains real.** Full-vs-chunk reference agrees; reference-vs-
   llama exceeds tolerance. No basis to dismiss it as chunking or certify runtime.
10. **Evidence scope is small.** One novel, sequential questions and a small set
    of diagnostics do not establish broad product reliability.
11. **Author intent is a hard constraint.** A factual-answer remedy must not turn
    speculative writing suggestions into source truth or silently delete context.

## Tensions and dependencies

- N2 ↔ N3: improving selection can repair missing evidence, but cannot explain
  wrong answers when the relevant original is already present.
- N4 ↔ N7/N11: restricting elaboration may reduce invented claims but must not
  become a global prohibition on the creative assistance the author requested.
- N5 ↔ N10: a positive removal result is informative but underidentified;
  matched-token controls are needed before claiming semantic contamination.
- N6 ↔ N9: arithmetic and sampler effects must be separated from prompt effects;
  otherwise a successful wording change can conceal an uncontrolled runtime change.
- N1 → N2 → N3 → N4: each link is necessary in some tasks, none alone sufficient.

## Candidate investigations, not production decisions

- Same-route repetition-penalty pair with all other fields frozen (N6).
- Per-message token-matched neutral history, with template parity checked (N5).
- Full context plus a minimal factual answer-length constraint; separately an
  exact-quotation contract (N4/N7). No prompt is promoted based on this case.
- Matched-prefix CPU/Metal/reference arithmetic comparisons if the numerical
  gap is needed to explain failures that remain after these controls (N8/N9).

**Complexity check:** continue. Multiple independent failure locations and
confounded explanations remain; neither a single obvious patch nor a model
switch is warranted by the current evidence.

## Reopened plumbing nodes after user challenge

12. **Shared-input confound:** independent engines can share an application-
    composed protocol mistake. Node 8 cannot exclude that class of plumbing.
13. **Deployment contract mismatch:** native thinking/sampling settings differ
    from the pinned model card. A deliberate override is not inherently a bug,
    but no same-input recommended-contract control has ruled it out.
14. **Arithmetic/cache localization:** the known >0.12 gaps need CPU versus
    Metal, BF16 versus F32 KV, and cold-prefill versus incremental comparisons
    on identical prefixes. Existing checks do not identify which is responsible.

Dependency: establish the arithmetic/control boundary (14) separately from the
deployment/task interface (12/13), so improvements are not misattributed across
both layers.
