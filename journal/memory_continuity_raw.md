# LMM RAW — memory as a writing-session capability

Scope: explore the problem space and competing solutions before building. These
are observations, hypotheses, questions and candidate directions, not a private
deliberation transcript. Preserve the failed frozen V2 evaluation. Do not enable
memory, add T2, change model providers, expand consent or modify author originals.

The runtime probes demonstrate that stored evidence can be correct and present
while its arrangement makes the model behave as if it were absent. They also
demonstrate that factual correctness, citation compliance and successful brief
generation are separate properties. The experiment's 64 repeated user messages
are valuable stress, but insufficient as the sole model of a writing session.
None of this establishes that a larger model, a state database or a new prompt
will solve the actual product objective.

The objective is an author continuing work without rebuilding the shared
understanding every few turns. What does that understanding contain? Facts,
perhaps, but also rejected ideas, temporary alternatives, current intentions,
style constraints, unresolved questions, and the next planned action. A writer
can deliberately retain contradictory accounts. An assistant's proposal can
remain useful without ever becoming an author's decision. A short reply such as
“yes, but not the ending” is not meaningful without the preceding proposal.
Fixed byte chunks and isolated factual statements may preserve source integrity
without preserving these relationships.

The original MEMORY-002 design explicitly requested continuity, decisions and
open questions. The implementation's generic facts array omits that target.
Its failed weather brief faithfully repeats sentences yet does not identify what
will matter on the next turn. That is not merely a malformed-output problem.

Candidate directions include better native history composition, concise source
handles, original-first query-specific evidence packets, optional episode briefs,
author-visible working-state cards, a durable claim ledger, a larger local model,
or no generative summary layer. Each has costs. A ledger can harden a mistaken
extraction into persistent false belief. Compression can erase uncertainty.
Grouping history can destroy conversational adjacency. Query-specific generation
can violate the preparation budget. A stronger model can violate the pinned
runtime/resource contract. Dropping hard evaluation cases can conceal brittleness.

Open questions: which continuity tasks actually fail on native inputs? Is the
limiting factor source selection, relationship preservation or answer execution?
Can a useful bounded representation be assembled without another model call?
When should memory ask for clarification rather than choose a story version?
What should survive an hour, a restart, a retcon or an explicit scope change?
Can we demonstrate a benefit from T1 after charging generation, retries and
coverage gaps, rather than assuming the existence of summaries is progress?

The investigation should distinguish what host code can guarantee—scope,
identity, ordering, eligibility and exact quotation—from interpretations that
remain fallible. It should also define when to stop adding machinery if this
checkpoint cannot perform the required bounded task reliably.
