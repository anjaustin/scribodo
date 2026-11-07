# LMM REFLECT — a working situation, not a growing belief store

## Core insight

Memory should reconstruct a small, source-backed working situation for the
author's present task, not accumulate a purportedly authoritative world model.

## What changed after testing

The first solution candidates emphasized better formatting, typed state cards
and compact briefs. New continuity probes show that a hand-written *correct*
card is not a reliable cure: it can encourage the model to reproduce a card,
invent explanatory details, or begin drafting instead of answering a status
question. In the deliberate-ambiguity case, a stale generated brief labelled
noncanonical can bias the answer toward one account even with the original
author constraint present. Presence of originals and a warning label are not
semantic verification.

The most important distinction is not another field named `open_questions`.
There are at least two kinds of unknown: **not yet decided** and **deliberately
left unresolved**. Treating the second as unfinished work changes the author's
intention. Similarly, a proposed option is useful memory without being a fact
or a decision. Correct prose can still be the wrong assistance when the current
request asks for recall rather than brainstorming or drafting.

## Resolved tensions

### Compression versus fidelity (N2/N3/N7)

Compress relationships, not merely sentences. Preserve who proposed an idea,
whether the author accepted/rejected it, and the relevant counter-statement.
Use bounded adjacent-original packets before introducing a durable extracted
claim ledger. Any typed interpretation remains fallible and source-linked;
it never becomes canon or a source of authorization.

### Better organization versus lost dialogue meaning (N4/N5)

Test deterministic grouping of consecutive history while preserving exact
speaker/order and adjacency. Do not manufacture assistant replies or flatten a
proposal and its rejection into independent facts. Keep the current author
request explicit and protected. Grouping is a candidate renderer, not a proven
universal fix: reference packets themselves can induce the wrong response style.

### Safety labels versus useful author intent (N3/N6)

Earlier author directions are evidence about what the author wanted, not new
system instructions. Current author requests may supersede them. A quoted
manuscript instruction is not a project constraint. Role/source/context labels
help interpretation but cannot guarantee it. Scope, source admission, provider
boundaries and mutation authority remain host-enforced; no recalled text is
promoted into system/tool authority.

### Progress versus truthful coverage (N8)

Keep processing, retained-useful-information and retrieval coverage separate.
An empty valid brief or a fully advanced processing frontier does not establish
that important decisions survived. A failed brief must not silently advance
coverage; it also should not make validated original-source recall unavailable.

## Further implementation and information-contract findings

1. `memory_plan_add_briefs()` stages changes without disturbing a valid raw
   plan on failure, but `chat.c` assigns its return value to `available`. Thus
   an optional brief-read/validation failure can suppress raw recall that already
   succeeded. A future remediation must preserve raw fallback while still
   revalidating source/scope at final admission. This is a code-path finding,
   not a newly reproduced native integration test.
2. T1's model-facing renderer includes three full hashes for the brief plus
   three hashes per original pointer. Four inputs mean **15 64-character hashes
   (960 hex characters)** before other metadata or summary text. A small summary
   payload does not prove a smaller *rendered* context. Keep full proof metadata
   in the host's immutable receipt and expose short bound handles to the model.
3. A paraphrased brief and source IDs do not contain the original wording needed
   for a guaranteed exact quotation. Quote/dispute tasks must select original
   text within the shared budget. Continuity retention and quoteability are
   different capabilities. This is a latent design/evaluation constraint, not
   the cause of V2's measured failure: that run published no T1 briefs.

## Assumptions rejected or qualified

- More verified evidence necessarily improves answers: false in prior composition
  probes; a stale derived aid can also contaminate an otherwise adequate packet.
- A facts list is equivalent to continuity: contradicted by the product target
  and by decision/ambiguity tasks.
- Correct state cards solve model behavior: damaged by the new oracle-card
  controls; extraction quality is an additional unmeasured problem.
- All unknowns should be resolved: wrong for deliberate ambiguity.
- Runtime fidelity implies writing-task capability: not established.
- A full token window is useful context: not established; use minimum sufficient
  context as a testable objective, not “fill the cap.”

## Refined direction

First improve native context composition, explicit source handles and independent
raw fallback. Evaluate continuity/task/authority fidelity before redesigning T1.
Then test one bounded, episode-aware brief against a no-summary baseline. Defer
a semantic ledger, automatic mode classifier, T2 and model changes until an
experiment identifies which additional capability is actually missing.
