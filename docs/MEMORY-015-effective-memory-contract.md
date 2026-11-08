# MEMORY-015 — Effective memory: the contract and the P0 plan

**Date:** 2026-11-07

**Status:** authorized direction. On 2026-11-07 the author made Memory the P0,
agreed with the canon-first assessment in [MEMORY-014](MEMORY-014-canon-first-continuity.md),
and set two standing rules for this workstream: **memory needs special care**,
and **delete nothing, only archive**. This document is the contract the work is
held to. It changes no production routing, prompts, tiers, consent or defaults
by itself.

## 1. The goal, in one sentence

Give LFM3 **infinite effective memory, not an infinite context window.**

The archive of what the author wrote and decided grows without bound. The
model's window stays bounded by measured memory on this Mac. The system's job is
to keep everything addressable with exact provenance and, for each request,
select the slice that this turn depends on. The model never needs to hold the
book, only the part of the book this turn needs.

## 2. Where the idea came from

The lineage is recorded and should be read in this order:

1. `journal/manuscript-memory_*.md` and
   [`reports/How billion context works.md`](../reports/How%20billion%20context%20works.md):
   the research pass on `ranxianglei/billion-context`. Its thesis is that
   long-session memory is a coordinated lifecycle: identity, coverage, protection
   and rendering managed by a kernel; summaries written by models; originals
   kept recoverable by storage.
2. [MEMORY-001](MEMORY-001-long-lived-context.md) adopted four ideas from it and
   rejected the proxy itself. The adopted ideas: originals stay authoritative;
   summaries are a separate layer that never becomes canon; exact source
   passages are retrievable; host work is bounded, not only tokens. The decisive
   number was verbatim recall of 27% under compression against 69% with full
   context in the upstream benchmark.
3. [MEMORY-002](MEMORY-002-mvp-design.md) chose a native MariaDB and Qdrant
   planner over reuse. MEMORY-003 through 009 built it: bounded history, paged
   sources, fenced ingestion, saved-document scope, resource bounds, gated T1.
4. MEMORY-010 through 013 ran four frozen experiments against the pinned model.
   All failed the continuity screens, and MEMORY-012 located the failure in
   interpretation and authority, not in retrieval, template, tokenizer or
   runtime.
5. MEMORY-014 drew the conclusion: keep the storage, provenance and exact
   retrieval half of the inspiration, which the research always trusted; drop
   the model-authored summary half as the authoritative layer, which the
   research always doubted. Canon becomes the mechanism that turns implied
   intent into stated fact, with the author as the inference engine.

The research anticipated this. The evidence note recorded that a 27B model
restored originals six of six times when instructed and adopted the behavior
spontaneously zero times, and that the upstream evidence "does not establish
small-model fiction-memory quality."

## 3. The decomposition

Effective memory has three parts. They are not equally hard, and conflating
them is what produced four failed cycles.

| Part | Meaning | Status | Owner |
|---|---|---|---|
| **Retention** | Nothing the author wrote or decided is lost; everything has exact provenance | Done. MariaDB originals, immutable revisions, canon versions, Qdrant as a rebuildable index | Store |
| **Selection** | For each request, the right slice reaches the window | Solved for stated facts; unsolved for implied intent, which exists only as a pattern across turns | Host: the main P0 engineering track |
| **Use** | Given correct evidence, the model answers faithfully | Failed for authority, ambiguity and scope with this model | A measured gate, not a thing to fix with prompts |

Canon converts implied intent into a stated fact. It moves work from the
"use" column, where the model fails, to the "selection" column, where the host
is reliable.

## 4. The promise

What the product promises:

- Nothing is forgotten. Every message, revision, note and canon version is kept
  and addressable.
- What the author has **stated** reaches the model when it is relevant, with
  exact text and source.
- The author can inspect what the system remembers, correct it, and see what it
  searched and what it did not find.
- A model error can degrade a draft or a conversational aside. It cannot
  corrupt the record of what the author decided.

What the product does not promise:

- Automatic capture of decisions the author never stated.
- Perfect recall, or manuscript-wide absence ("this never happens in the book").
- That the model will use correct evidence correctly. That is measured and
  disclosed, never assumed.

## 5. Decisions recorded on 2026-11-07

- **Canon is project-wide for the recall view.** An author expects the record
  of the book to be the record of the book, not of one chat. Per-conversation
  consent still governs anything that enters a model request. This resolves
  MEMORY-014 open question 2 for P0 only.
- **Delete nothing, only archive.** Evidence cited by dated reports is archived
  per [EVIDENCE.md](EVIDENCE.md). Code that carries a failed experiment is
  archived in place: the compact and episode presentations stay in the tree,
  tested, idle-only and flag-gated, because the native eval harness exercises
  them and MEMORY-011 cites them. They are not on the authorized path and must
  not be extended. Any future removal is a move into the harness, not a delete.
- **Two metrics, measured separately.** *Reach:* did the right source reach the
  model. *Faithful use:* did the model use it correctly. No aggregate mean may
  combine them or hide an authority violation.
- **Fixture first.** Every quality experiment so far used toy facts. Before
  selection work is tuned, a realistic novel-length fixture with planted facts,
  decisions, revisions and a long chat history is built, with known answers.
  It is synthetic and reproducible unless the author supplies a manuscript,
  which stays on this Mac.

## 6. P0 slices, in order

Each slice is default-off until its gate passes. Each gets a dated record.

**S1 — Deterministic recall view.** *Implemented 2026-11-07; record in [MEMORY-016](MEMORY-016-recall-view.md).* MEMORY-014 §5 P0. A host-rendered surface
that takes a query and renders matching committed canon (exact current text,
version, status) and matching original excerpts (exact text, source, revision
or message, range), plus a coverage line: what scope and how many entries were
searched, and whether nothing matched. No model call on this path. Reuses the
canon record reads, committed-canon ranking and the existing admission path.
Gate: MEMORY-014 §8.1, every axis. False-authority incidents on this surface:
zero by construction.

**S2 — Realistic fixture and the two-metric harness.** *Implemented 2026-11-08; record and first measurements in [MEMORY-017](MEMORY-017-reach-harness.md).* A reproducible
manuscript-scale project with a long stored chat, planted stated facts, implied
decisions, revisions that supersede facts, retracted canon and protected
ambiguities. The harness measures reach (does the planner admit the unit that
holds the answer, under production budgets) and, separately, faithful use (the
existing native eval against the pinned model). Gate: the harness reproduces
the known MEMORY-011 failures and passes its own negative controls.

**S3 — Selection improvements.** *2026-11-08: the fusion constant, lexical length normalization and case-insensitive lexical matching, all measured in [MEMORY-018](MEMORY-018-selection-ranking.md).* Host-side only, each measured against S2:
canon-aware ranking as a separate reference class; revision and recency
awareness; author-marked importance; project-wide canon scope under
per-conversation consent. Gate: reach improves on S2 with no regression in the
scope, privacy and cost axes.

**S4 — MEMORY-003 leftovers.** *Baseline and first remediation (asynchronous import) 2026-11-08 in [MEMORY-019](MEMORY-019-manuscript-baseline.md).* Windowed chat loading in `open_chat`, stable
source identity through eviction, and a latency and memory baseline on the S2
fixture. These predate the retrieval work and are prerequisites for any
sustained-use claim under GAP-003.

**P1 and beyond.** Canon-aware chat (MEMORY-014 §5 P1) starts only after S1,
with its own frozen registration, rubric and stop condition. It is a new
contract, not a retune of a failed fixture. *First run 2026-11-08 in
[MEMORY-020](MEMORY-020-canon-aware-chat.md): canon removed the authority
and open-state failures it targeted and introduced over-anchoring in four of
27 alternating-layout cases; the stop condition was met, P1 does not advance
in this form, and the fixture is not retuned. P0 stays authoritative and
the model non-authoritative, per MEMORY-014 §9.*

## 7. Guardrails that apply to every slice

- No prompt or presentation change ships without a measured old-versus-new
  sample on the real model.
- No model edge into canon, evidence or retrieval eligibility.
- Scope and consent apply identically to canon and evidence. Canon never widens
  access.
- Generated aids stay labeled noncanonical and off the authoritative path. T1
  stays default-off; T2 stays deferred.
- Absence is stated as bounded coverage, never as a fact about the manuscript.
- Mechanical test success is not a quality claim. Dated reports carry the
  numbers; living docs carry the contracts.
