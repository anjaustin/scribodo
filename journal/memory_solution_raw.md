# RAW — the continuity problem, chopped open

The thing we keep calling "memory" may not be a memory problem at all. We have
spent four cycles trying to make a local model conserve author state: retrieval
of exact originals, episodes, compact handles, generated T1 briefs, a frozen
V2 harness, a 42-answer native run, 94 causal generations, an explicit
Recall/Draft split. Every automatic path failed. The model often restates the
right sentence and then draws the wrong conclusion about it. It can say "cobalt
is only an option" and still answer as if cobalt were chosen. It can quote the
author and then attribute the author's choice to itself. That is not a retrieval
miss. It is a failure of semantic authority.

So my first gut reaction is: stop asking the model to be the authority. But
that sounds like giving up, and the user says the solution is waiting. What if
the solution is not a better model or a better prompt, but a different place to
put the truth? We already have a place: canon. `records.h` says canon entries
are "explicit, author-approved assertions" and "nothing becomes canon except
through records_append() called for an explicit author action." They are
versioned, they carry source references with quotes, they can be retracted, and
the observer's findings already have a "Commit to canon" button. That is a
ratification pipeline. We built it. Then we built a second, generative memory
path beside it and tried to make that one reliable.

Why did we do that? Because canon is manual, and "AI memory" promises magic.
But the magic is exactly the part that failed. The manual part is the part that
is mechanically trustworthy. Maybe the product mistake is treating canon as a
side feature for worldbuilding facts while treating chat memory as the real
continuity system. Invert that: canon is the continuity system; chat is a
drafting and proposal surface.

Doubts. Authors may not curate. An empty canon is useless. The observer may not
notice the decisions that matter. Canon entries are still text, and a model can
misread them. Putting canon in the prompt is not a proof of fidelity. And a
host-rendered recall panel is not "AI"; it may feel like a regression to search.
Also, the earlier oracle-card experiment showed that hand-written continuity
cards sometimes caused the model to write about the card instead of the task.
So even exact author text in context is not automatically safe.

What is actually reliable? Bytes, identities, versions, order, scope, consent,
hashes, token counts, exact quotation. What is not? Meaning, entailment,
intent, whether a paraphrase preserves a commitment, whether "maybe" was a
decision. The boundary is verification asymmetry. Any design that routes a
semantic commitment through generated text creates an unverifiable dependency.
Any design that keeps the commitment in author action or exact quotation is
verifiable by a human in one glance.

Open questions I cannot answer from the current evidence:

1. If the authoritative recall surface is deterministic and host-rendered,
   does the model's semantic unreliability stop mattering for the product
   promise, or does it merely move the failure into the conversation?
2. Will authors actually mark commitments, or is curation friction the real
   reason automatic memory is attractive? If they will not, is the honest
   answer "no automatic continuity yet" rather than a cleverer extractor?
3. Does committed canon in the prompt improve model behavior enough to be
   worth including, or does it become another authoritative-looking artifact
   the model can misread?
4. What is the smallest author action that creates enough continuity? A pin on
   a span? A one-line decision? A commit of an observer finding?
5. Are we allowed to call a deterministic evidence view "memory" if it does
   not generate? Or is the product promise actually "recall I can trust,"
   which generation was never required to satisfy?

The phrase "the solution is waiting for us" makes me think the answer is
already in the repository, unused. Canon is in the repository. `semantic_canon_for`
searches committed canon, but only the observer calls it. Chat memory never
touches canon. We keep building new semantic layers while the author-approved
one sits outside the conversation. That is a suspiciously strong signal.
