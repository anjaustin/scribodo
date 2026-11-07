# REFLECT — the structure beneath the nodes

## The one-sentence insight

**Reliable continuity requires that the author's own committed words — not the
model's inferences — be the authoritative memory, and that this memory be
rendered deterministically rather than regenerated.**

## What the structure actually is

Strip the four experiments down and one pattern remains. Every failed design
put generated text on the path between evidence and commitment:

- generated T1 brief between originals and answer;
- model status label between evidence and author state;
- model-authored Recall prose between the source and the author's decision;
- model-authored Draft prose that could later be mistaken for prior context.

The evidence layer was never the weak part. Admission, provenance, receipts,
scope, and exact quotes held. The weak part was always the *transformation* from
evidence to meaning. The causal work showed that the transformation is not a
storage problem and not reliably fixed by task labels. So the correct move is
not to strengthen the transformation. It is to remove it from the authoritative
path.

That is a structural inversion:

```
old:  evidence → model interpretation → author sees model's meaning
new:  evidence → author-ratified canon → host renders exact meaning
                    ↑
        model proposals (never authoritative)
```

The model is still useful. It proposes canon entries from findings, drafts
prose, answers conversational questions, and can summarize. But none of that is
where the author's continuity lives. Continuity lives in text the author has
vouched for, with sources attached.

## Tensions resolved

**T1. Automation vs authority (N8).** Resolve by splitting the pipeline, not by
compromise. Automation owns *proposal* and *drafting*. Authority owns
*commitment*. The system can be aggressively automatic at proposing and still
keep canon clean. This is not "manual memory"; it is automatic suggestion with a
one-action ratification gate. The gate is the feature.

**T2. Prompt inclusion vs host display (N9).** Host display wins. Canon in the
prompt is an optional quality improvement; the host-rendered view is the actual
guarantee. This ordering matters: if you only put canon in the prompt, you have
recreated the same dependency on model interpretation with a new input. If the
authoritative surface is deterministic, a model misreading canon is a
conversational error, not a memory error.

**T3. Canon vs evidence (N12).** They are different layers with different jobs.
Canon says "the author asserts X." Evidence says "the text says Y at this
version and range." Recall should show both: the assertion and the source it
cites. Quotation and dispute tasks must use evidence, not canon paraphrase. This
also preserves the author's ability to be wrong, change their mind, or retract.

**T4. Coverage vs confidence (N10).** Make coverage a host fact. "I searched
committed canon and N excerpts in this project/version; no match" is verifiable.
"Nothing exists in the manuscript" is not. The model should never be asked to
infer absence.

## Hidden assumptions surfaced

**H1. "Memory must be generated to be useful."** False. Much of what authors
need is *finding* and *showing*, not *summarizing*. A deterministic recall
surface can be more useful than a fluent wrong answer because it is inspectable.

**H2. "If the model is unreliable, the product has no AI continuity."** Only if
continuity is defined as model-authored prose. If continuity is defined as
reliable access to the author's own decisions plus exact evidence, the product
can deliver it today with existing components.

**H3. "Canon is for worldbuilding facts."** Canon is for any author-approved
assertion: decisions, constraints, protected ambiguities, rejected options,
voice rules. The schema is content-neutral. The limitation is the capture UI and
the retrieval wiring, not the model.

**H4. "Putting the right text in context is enough."** The experiments refute
this. Correct context plus a model that must interpret authority still fails.
Context is necessary, not sufficient; the answer surface must not depend on the
interpretation.

**H5. "Explicit ratification is too much friction."** Possibly, but the current
alternative is invisible, authoritative-looking errors. The honest comparison is
one click per important commitment versus silent canon corruption. The
observer/finding pipeline and the chat "Canon" action can make the click
cheap. Whether authors accept it is an empirical product question.

## What changed in my understanding

I began thinking the task was to make the model trustworthy enough to remember.
I now think the task is to stop placing trust in the model at the point where
trust is unrecoverable. The model can be wrong in a draft; the author sees the
draft. The model must not be wrong in the record of what the author decided,
because the author may never see the error. The architecture should make the
second kind of error structurally impossible, not statistically rare.

This also explains why the earlier "narrower author-controlled workflow" note
kept reappearing. It was not a fallback. It was the shape the evidence was
pointing at all along.

## What this does not establish

It does not show that authors will curate canon, that canon-in-prompt improves
chat, that the recall UI is pleasant, or that the local model is adequate for
drafting. It does not qualify T1/T2 or automatic continuity. It is an
architecture hypothesis with a strong mechanistic rationale, not a passed test.
