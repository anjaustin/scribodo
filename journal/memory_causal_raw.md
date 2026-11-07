# Causal investigation — problem and evidence inventory

## Scope

Find the earliest reproducible cause of continuity failures, rather than fitting
another memory representation to the observed answers. Preserve the previous
no-go, original sources, sample edit, frozen DEV results, privacy boundaries and
resource limits. This phase is diagnostic; it does not authorize a model switch,
new memory tiers, or automatic changes to author decisions.

## Observations and competing explanations

The tested system admits the relevant originals and preserves the current
request, yet sometimes answers an older repeated weather turn. Compact rendering
helps less than grouping consecutive users. Grouping changes token layout and
message boundaries, and costs additional tokens; it is not clean evidence that
less information or shorter context fixes the problem. Alternating histories
still exhibit first-person speaker confusion, unsupported resolution plans,
false citations and output-length violations. These may be different causal
families rather than one memory deficiency.

Known controls narrow the possibilities without closing them. Raw completion
reproduces chat behavior from llama-rendered tokens, but both still share the
same tokenizer, template, GGUF interpretation and inference engine. CPU/Metal
agreement excludes a GPU-only explanation, not a shared implementation defect.
Earlier runtime parity was measured on other prompts and cannot substitute for
parity on the actual failing inputs. The official checkpoint and reviewed model
code are already cached locally, but the former model-check environment is no
longer present.

The model also receives a creative-writing persona, quoted first-person author
text, reference warnings, source handles and a task that may ask for literal
recall. A correct latent interpretation could be lost during answer composition;
alternatively, the interpretation itself could already be wrong. A correct
forced-choice classification would not establish reliable free-form assistance,
but it would distinguish these mechanisms. The immediate research priority is
to avoid conflating runtime fidelity, evidence interpretation and task execution.

## Open questions

1. Do official-template tokens match llama.cpp tokens on the *failing* inputs?
2. Does the independent reference reproduce their semantic failures under matched
   greedy decoding, and where do their predictive distributions first diverge?
3. Can the model correctly classify who proposed/accepted/rejected a choice when
   that is the sole task, before asking it to generate helpful narrative prose?
4. Does a minimal recall instruction versus the writing persona change behavior
   while the evidence remains unchanged?
5. Which apparent failures disappear under a narrower task, and which survive
   even a single short transcript with an explicit question?
