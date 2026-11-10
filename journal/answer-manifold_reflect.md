# Answer reliability manifold — REFLECT decision summary

## Core insight

**The product must preserve the binding between the author's question, the
eligible evidence and every answer claim; storing and supplying the right words
does not establish that binding.**

This is a testable organizing model, not an architectural cure. Its advantage is
that selection, model interpretation and acceptance can be tested separately
without inventing a new persistent memory tier.

## Resolved tensions

- **Selection versus interpretation:** retain both as independent contracts.
  Income demonstrates a selection loss; Collins supplies a counterexample to
  the claim that selection is the whole explanation. A selector cannot earn an
  answer-quality pass merely by delivering a passage.
- **Restrained answers versus creative collaboration:** measure narrow contracts
  on explicit factual questions, without globally restricting the creative
  persona. Treat suggestions, quotations, source claims and author corrections
  as different output obligations, not one generic answer score.
- **Semantic failure versus numerical uncertainty:** shared-prefix errors show
  that some failure is not exclusive to the production backend. They do not
  make the measured numerical discrepancies disappear. Keep the arithmetic
  question open rather than making all progress contingent on perfect parity.

## Assumptions to challenge

1. A known correct answer in a response implies success. Counterexample: a
   response that also invents hereditary property or misquotes its source.
2. A smaller context proves history was semantically misleading. Counterexample:
   the removal also changes positions and length.
3. Temperature zero removes decoding effects. Counterexample to test: repetition
   penalties still change scores even with deterministic selection.
4. The single-source failure establishes inability to read the fact. Counterclaim:
   the fact was correctly named; the failure may be unconstrained elaboration.
5. A stronger prompt solves memory. Counterclaim: it cannot recover evidence
   that the retrieval planner never admitted and may fail on corrections/absence.

## Change from the initial diagnosis

The phrase “model interpretation failure” remains too coarse. The next tests
separate **fact selection**, **unrequested elaboration**, **citation binding**,
**history content**, and **decoding**. A response can succeed on the first and
fail on the others. The smallest useful next result is a discriminating control,
not another broad claim that the model is either capable or incapable.

## Verification plan

Run the same captured Collins chat request through one owned local server, with
actual 8K slots, cold requests and the full 2,048-token reply cap. Predeclare:
native-penalty baseline; penalty 1.0; two independently worded token-matched
neutral histories; full context plus “Answer in one sentence.”; full context
plus an exact-quote-only contract; and the existing bound single-unit control
with the same one-sentence constraint. Preserve every complete response.

Evaluate core fact, extraneous factual claims, verbatim quote fidelity, source
binding and completion separately. Compare with earlier complete replies, but
do not call different caps/routes a controlled penalty experiment. If a minimal
constraint fails, do not accumulate prompt rules until the fixture passes.

## Refined after adversarial measurements

The full-context one-sentence arm supplies a counterexample to a broad
“cannot read the supplied fact” diagnosis. Both same-length neutral histories
remain wrong, so the previous false income answer is not necessary to trigger
this failure. One-sentence counterfactual output follows telescope rather than
canonical living: constrained source-following capacity exists in this narrow
test. None of this establishes general reliability or restores missing evidence.

The review process itself needed a correction: raw self-correction in the
synthetic living quote is removed by the actual product's thinking cleanup,
leaving faithful visible support. Evaluating provider content alone would have
incorrectly failed that visible answer. Keep **generated tokens**, **visible
claims**, and **source/claim fidelity** as separate outcomes.

Final instruction-specificity and cross-question controls are predeclared in
the turbo record; do not promote the positive Collins arm before inspecting
those results. The numerical runtime gap and retrieval budget loss remain
independent open work.

One additional causal boundary matters: **a designated-passage miss is not by
itself proof that the miss caused the wrong answer.** Income's admitted later
passage contains the correct range. A passage-replacement intervention under
fixed decoding would be needed to establish how much the missing opening source
causes that answer error. Partial recall is disclosed by the product; rank 9
outside an eight-unit cap is a measured coverage limitation, not evidence of
SQL/vector corruption or an implementation violating its stated cap. Keep the
coverage target and the causal answer-quality claim separate.

## After final falsification

Equal-length non-directive suffixes remain wrong while the one-sentence Collins
answer replicates exactly; the local task-content effect survives that challenge.
Transfer breaks the global remedy: income is short but wrong, other replies
ignore the constraint or misattribute sources, and absence handling still loops
to the reply limit. The synthesis therefore recommends neither a global sentence
rule nor history removal. It narrows the next cycle to source-to-claim task
execution, with explicit tests for answerability, contradictions and author
intent, while preserving the separate coverage and arithmetic investigations.

## User challenge: shared-runtime errors do not clear shared plumbing

The user is right to challenge a capacity interpretation. The independent
reference consumed the application's **same composed request**, including its
history, evidence representation and task/decoding assumptions. Agreement on an
error does not establish that the caller is using the model correctly. It only
narrows an explanation unique to one backend for that shared prefix.

Reopen in a plumbing-first order: localize arithmetic/cache discrepancies; compare
the pinned deployment defaults with the actual request without prompt edits;
then test whether the reply cap prevents a final answer. Keep model capacity
claims suspended. Any override of a vendor default is a measured difference,
not automatically the root cause; each intervention must earn that attribution.
