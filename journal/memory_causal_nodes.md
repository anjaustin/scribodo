# Causal investigation — nodes

## Observations

1. **Transport integrity:** admitted originals and current request are present.
2. **Layout sensitivity:** repeated consecutive users fail much more often;
   deterministic grouping helps without eliminating semantic failures.
3. **Authority binding:** source speaker and author commitment are not reliably
   preserved in the generated answer.
4. **Task binding:** recall can become drafting or a reply to stale filler.
5. **Reference binding:** model citations can be wrong despite valid host receipts.
6. **Shared-runtime blind spot:** earlier raw/CPU controls share the same engine;
   earlier official-reference checks did not use these failing cases.
7. **Composition versus competence:** a model may distinguish a state in an
   isolated classification but fail to preserve it during helpful prose.
8. **Experimental integrity:** fresh diagnostic controls are not new held-out
   evidence; do not change production behavior or score away previous failures.

## Dependencies and tensions

- N3/N4 may be downstream of N6. Establish exact-input runtime/template parity
  before labeling all failures learned-model limitations.
- N7 conflicts with interpreting a good forced-choice answer as product quality.
  Such an answer locates a failure boundary; it does not qualify assistance.
- N2 groups message boundaries and token positions together. A claim about
  conversational training alone requires more discriminating controls.
- N8 limits the response to a negative result: reduce the hypothesis space,
  not broaden an unqualified architecture.

## Diagnostic paths, not selected product solutions

First compare exact failing prompts under independent official inference.
Then isolate interpretation from generation using short, paired synthetic
proposal/ambiguity tasks with unchanged evidence and explicit task interventions.
Only an observed causal contrast can justify a proposed remedy.
