# MEMORY-021 — A real manuscript through the memory paths

**2026-11-08. First measurements of selection reach and the whole application on a real, book-length text rather than template prose.**

**Status:** measured on this Mac with one public-domain novel. The reach
figures are measurements against hand-written answer keys, not a pass; the
template-prose limit stated in [MEMORY-017](MEMORY-017-reach-harness.md) is
now a measured difference.

## The text and how it enters

*Pride and Prejudice* (Project Gutenberg #1342, plain text, SHA-256
`3f6bb9d6…caf9d75` recorded beside the import), 61 chapters, 121,787 words,
694,456 bytes after import. `tools/manuscript_import.py` strips the Gutenberg
header and footer, drops the illustration captions (keeping the one chapter
heading printed inside a caption), splits at the chapter headings, unwraps
the hard line breaks and writes one `chNN.md` per chapter plus
`MANUSCRIPT.json`. With `--parts 8` it also groups the chapters into eight
consecutive parts of 63 to 96 KB, because a conversation can attach at most
`MEMORY_DOCUMENTS_MAX` (8) saved documents, and memory covers a
conversation's attached documents, not the whole project. That cap is the
first product fact a real book exposes: a 61-chapter novel cannot be attached
chapter by chapter.

The imported text is kept under `build/manuscripts/pride-and-prejudice/`
(archived per [EVIDENCE.md](EVIDENCE.md), not committed); the import is
reproducible from the source file and the tool.

## Reach over real prose

Run `scribodo_test_reach_15740_531691605f868f57`: the production planner and
admission, 16 lexical and 16 semantic candidates, 8 admitted excerpts, 4,096
reference tokens, real Qwen3 embedder and LFM3 tokenizer. The conversation
is the 30K fixture chat (402 turns, the planted chat tasks unchanged) with
the eight parts attached; 807 units ingested and indexed in 113 s. Fourteen
hand-written `document_fact` tasks
(`tests/fixtures/memory_reach/pride-and-prejudice-tasks.json`), each with
the exact sentence from the part that answers it and the words a correct
reply must carry; the runner refuses a key whose text is not in the part.
All harness gates passed.

| Tasks | reach@candidates | reach@admitted | lexical hit | semantic hit | whole sentence admitted | planning p50 | planning p95 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 14 document tasks over the novel | 1.00 | **0.71** | 0.93 | 0.93 | 0.71 | 113 ms | 141 ms |
| 24 planted chat tasks, same run | 1.00 | 1.00 | 1.00 | 0.96 | | 74 ms | 133 ms |
| for comparison: 4 template document tasks (MEMORY-017) | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 54 ms | 54 ms |

Every answer-bearing excerpt reached the 32 candidates. Four of fourteen were
not among the excerpts handed to the model. That is the first admission
failure any reach run has produced, and the chat tasks in the same run did
not move, so the attached book did not crowd out the conversation.

**Admission is budget-bound on real prose.** Every document task admitted
exactly 7 units and used 3,596 to 4,025 of the 4,096 reference tokens. A
real-prose unit is the full 2,000-byte cap, about 500 tokens; the template
fixture's planted turns were a tenth of that, which is why 8 admitted was
free there. With 7 places, the fused position of the answer unit decides.

**Where the four misses sat** (ranks are 0-based; fused = reciprocal-rank
fusion with k=1 over the two 16-candidate lists, as in the planner):

| Task | Question | Lexical rank | Semantic rank | Fused position | Why |
|---|---|---:|---:|---:|---|
| t29 | Bingley's income on arrival | not in 16 | 3 | 9 of 28 | The answering sentence shares no term with the question; the unit reached only through meaning, and a single-channel rank 3 fuses below units found by both channels. |
| t31 | What is Longbourn? | 11 | 15 | 13 of 24 | One query term, a proper noun that appears in about a hundred units; the defining sentence is not lexically distinct from any other mention. |
| t32 | How many daughters | 10 | 12 | 10 of 26 | "Mrs", "Bennet", "daughters" are among the commonest words in the book; the counting sentence ranks with the crowd. |
| t35 | Darcy's county | 6 | not in 16 | 13 of 30 | Lexical found it at 6; the embedder did not place it in its 16, and a single-channel rank 6 fuses low. |

The ten that were admitted sat at fused positions 0 to 6, found by both
channels. The pattern is consistent: on a real book the discriminating
signal is weak exactly when the question is about a frequent name or is
definitional, and the fusion constant that fixed the superseded-fact loss
on template prose ([MEMORY-018](MEMORY-018-selection-ranking.md)) now
penalizes an excerpt only one channel found.

## What this does and does not establish

- A real manuscript is harder than the fixture, by a measured margin:
  reach@admitted 0.71 against 1.00, planning p95 2.6 times longer and still
  well inside the 500 ms budget.
- The failures are at admission, not discovery, and not in the lexical
  index or the embedder individually: thirteen of fourteen were found by
  each channel.
- Fourteen questions over one novel is a first measurement, not a
  distribution. The questions are mine, chosen to be answerable from one
  sentence; an author's questions about their own book may be easier
  (they know their wording) or harder (they ask about arcs, not sentences).

## Candidate remediations, measured offline on the frozen runs

The frozen events hold both candidate lists with scores, so two of the
three levers can be replayed without the app or the model. Replayed on this
run and on the two post-cut template runs (MEMORY-017):

**Fusion constant.** Tasks whose required units all land within the first
N fused positions, by reciprocal-rank constant k:

| Run | tasks | k=1 (production) | k=5 | k=10 | k=60 |
|---|---:|---:|---:|---:|---:|
| novel, within 7 | 38 | 34 | 34 | 35 | 35 |
| novel, within 8 | 38 | 34 | 35 | 35 | 36 |
| template 30K, within 7 or 8 | 27 | 27 | 27 | 27 | 27 |
| template 100K, within 7 or 8 | 36 | 36 | 36 | 36 | 36 |

k=1 was chosen in MEMORY-018 when the lexical channel was broken and a
superseded fact had to be rescued; with the lexical fixes in place the
template runs no longer depend on it at all, and on the novel a larger
constant rescues one or two of the four misses. The constant is no longer
load-bearing on template prose, which makes it safe to re-set on real prose.

**Excerpting the unit.** If each admitted unit were cut to the sentences
containing a query term plus one sentence either side, the number of fused
candidates that fit 4,096 tokens rises from 8 to 10 whole units (this
estimate uses 4 bytes per token; the real tokenizer admitted 7) to 10 to 28
windowed excerpts, median 17. All four misses, at fused positions 9, 10, 13
and 13, fall inside the windowed cut; the two at 13 are inside it by a
margin that the 4-bytes-per-token estimate may overstate, so they need the
real tokenizer to confirm. This is the larger lever and it does not touch
the context size or the prefill cost.

**Term weighting by document frequency** was not replayed; it needs the
index, not the events.

Each remaining step is a software change with a before-and-after on the same
frozen runs, per MEMORY-015 §7. None is a prompt change.

## The application on the real text

`tests/ui/store/18_manuscript_questions.scribo`, driven by
`tools/manuscript_baseline.sh --manuscript <parts> --script …`, opens a
legacy project whose documents are the eight parts and whose chat is the 30K
fixture archive, attaches all eight parts to the chat, enables memory, and
asks seven questions whose answers are in the parts. Two hooks measure each
answer: `memory_sources` (the answer-bearing excerpt reached the model) and
`chat_last` (the reply carried the answer).

**First run, fake model: nothing was covered and nothing was sent.** The
eight parts attached (one click each), but `wait_memory_documents 8` never
settled and every question failed closed with "Memory admission changed or
exact context budgeting failed; nothing was sent." The run was repeated with
the database kept and sampled every 15 s (`--keep`, `tools/services.sh
testdb sql`). The chat ingested completely in the first sample (402 of 402,
640 units); the document side read `receipts 0` throughout, and the
`documents` table answered why:

```
name        has_saved  has_current
part01.md   0          0
…           0          0
part08.md   0          0
```

The legacy import recorded document rows and any snapshot history under
`.scribodo/revisions`, but never a revision for the file as it sits on disk.
A manuscript that arrives as plain files has no snapshots, so after import
its documents have no saved revision, and memory captures only documents
with a saved-revision identity (`sdb_saved_revision_identity`, verified
against the file's hash). The author would have had to open and save all 61
chapters by hand before memory could see any of them. That is a software gap
in the import, fixed in software: `import_saved_files` in `store_import.c`
now gives every document file a saved revision at import, reusing the
newest snapshot when its content is the file's and otherwise recording an
`import` revision dated by the file's modification time. `test_db:
test_import` covers both cases, and the rollback test now expects the
third, on-disk revision it previously lacked.

The "nothing was sent" message was a separate, harness-only effect: without
a model server the memory budget counts one token per byte while the request
builder estimates by bytes-per-token, so with eight large attachments the
exact budgeting step could never fit and failed closed. The real tokenizer
is used for both counts when a model is present, so this limits what the
fake backend can exercise, and the real-model run below is the measurement.

**Second run, real model: ingestion complete in 31 s, index halted at 3 of 806.**
With the import fix, the kept database showed all eight parts captured and
ingested (8 receipts complete, 694,517 bytes, 403 document units) and the
chat cursor at 402 of 402 within 31 s of launch. The index outbox then
stayed at 3 delivered and 803 pending for the rest of the run, and every
question was answered from the clipped heads of the attached parts, with
invented details (Mr. Bingley "twenty-nine", Lydia's regiment at "Meryton").
The worker's status line is the only place the reason lived, so
`memory_job.c` now also writes each index failure to the run log; the next
run read:

```
scribodo: memory Partial coverage: index failure 1/3; backing off: Invalid, oversized, or noncanonical memory unit
```

Two faults, both in software:

1. **My import revision id had the wrong shape.** The index validator
   (`mv_source_id`) accepts only the store's native id form,
   `r-YYYYMMDD-HHMMSS-` plus 16 hex digits; the import revision used the
   snapshot importer's derived `r-i…` form, so every document unit that
   pointed at it was rejected. The id is now built in the native shape from
   the file's modification time and a digest of the project and file name,
   deterministic so a repeated import is a no-op.
2. **One malformed unit halted the whole index.** The worker counted a
   rejected unit as a backend failure, and three backend failures stop
   indexing for the session until an explicit rebuild. That rule exists for
   an unreachable Qdrant, not for a unit that will fail identically every
   time. `memory_vectors_unit_valid` now runs before delivery; a unit that
   fails it is acknowledged as not delivered (the outbox backs it off and
   marks it failed after three attempts), counted in a new `rejected`
   status field, logged, and the drain continues. `test_memory_job` covers
   five rejections leaving the backend failure count at zero and the
   three-strike rule for real backend failures unchanged.

**Third run, real model, both fixes: the whole novel covered in under three
minutes, and the first answer came from the book.** Sampled every 15 s on
the kept database:

| Time from launch | Units in SQL | Indexed | Documents complete |
|---:|---:|---:|---:|
| 15 s | 132 | 65 | 1 of 8 |
| 45 s | 424 | 211 | 4 of 8 |
| 90 s | 806 | 434 | 8 of 8, chat 402 of 402 |
| 2 min 47 s | 806 | 806 | outbox empty |

Zero rejections, zero backend failures. The index kept pace with ingestion
at about 4.7 units per second with the LFM3 chat server and the Qwen3
embedder sharing the GPU (the app reported low memory and ran the chat
model at a 16K context). `wait_memory` and `wait_memory_documents` both
settled, and the question "In my opening scene, what does Mrs. Bennet tell
her husband about Netherfield Park?" produced a `memory_sources` receipt
containing "let at last" and a reply that quoted the opening passage of the
novel, including Mrs. Bennet's line, verbatim. Eight of eight script checks
passed; process wall 194 s including model start.

**Seven questions, real model.** The full script, same setup, 846 s of
process wall including model start, ingestion and seven replies:

| Question (part) | Excerpt reached the model | Reply carried the answer |
|---|:---:|:---:|
| Mrs. Bennet on Netherfield Park (part01) | yes | yes |
| Bingley's income on arrival (part01) | yes | **no**: quoted his inheritance of "nearly a hundred thousand pounds" from the same part, a true fact from the book but not the one asked |
| Lady Catherine's gift to Collins (part02) | yes | yes |
| Colonel Fitzwilliam's age (part04) | yes | **no**: "about sixteen", taken from the description of Miss Darcy that sits next to the answer |
| Georgiana at Ramsgate (part05) | yes | yes |
| The regiment's summer camp (part05) | yes | yes |
| Darcy's middle name (absence) | n/a | reply not asserted by the script |

Reach inside the application was 6 of 6, matching the harness. Faithful
use was 4 of 6: both misses are the model choosing a neighbouring true
statement over the one asked, with the answer-bearing sentence in its
sources. That is the model-side measure of MEMORY-015, not selection, and
it is the first such figure on real prose with the real model in the real
app. The two replies that missed also appended their own follow-up
questions, the known LFM3 habit from MEMORY-011.

Three of the application's defects surfaced only because the text was a
real book arriving as plain files: the attachment cap, the import without a
saved revision, and the index halting on one malformed unit. None of them
was reachable from the template fixture, whose documents were created
inside the store with native ids.

## The standard baseline on 61 real chapters

`tools/manuscript_baseline.sh --manuscript build/manuscripts/pride-and-prejudice`
runs the MEMORY-019 script unchanged against the 61 chapter files (121,787
words, 694 KB) plus the 100K fixture chat, fake model. The first attempt
failed at phase 2 for a harness reason: with 61 documents the chat list is
laid out below the explorer's visible area, and the script clicked the
off-screen rectangle. Screenshots showed the click landing on `ch28.md`,
the item at that screen position before the list moved. Element lookups now
scroll an off-screen explorer item into view, as a user would with the
wheel, and element clicks wait one frame so the layout applies the offset
before the pointer test. The full UI suite passes unchanged (398 checks).

| Phase | Wall | Frames | Worst frame |
|---|---:|---:|---:|
| 1 First open: import of 61 chapters and 1,116 messages (owned worker) | 1.07 s | 63 | 18.2 ms |
| 2 Open the 1,116-turn chat | 0.11 s | 6 | 18.2 ms |
| 3 Page older five times and back | 0.65 s | 36 | 18.3 ms |
| 4 Open a chapter, edit, save, revision recorded | 0.20 s | 10 | 33.0 ms |
| 5 Send a message (fake model) | 1.00 s | 54 | 42.0 ms |
| 6 Enable memory: ingest and index 1,116 turns | 54.9 s | 3,127 | 19.5 ms |
| 7 Recall over the archive | 0.28 s | 16 | 18.2 ms |

All 28 checks passed; app RSS peaked at 175 MB; process wall 59 s. Against
the MEMORY-019 baseline on generated chapters the figures are the same
within a frame, and the import stall that was 791 ms there is gone here
because the import now runs on its worker.

## Adversarial review

The changes this work made were red-teamed afterwards; the findings and
their remediations are in [REDTEAM-016](REDTEAM-016-real-manuscript.md):
the saved-file pass could block a project's move into the store, the
explorer scroll-into-view moved the explorer for toolbar buttons, and a
project moved into the store before imports recorded saved files had no
saved revisions for memory to cover.

## Evidence

`build/manuscripts/pride-and-prejudice/` (import, source and checksum, and
under `runs/` every application run's log, the database samples, the probe
scripts and the screenshots), and
`build/memory-reach/runs/scribodo_test_reach_15740_531691605f868f57`;
archived as `scribodo-evidence-2026-11-08c.tar.gz` per EVIDENCE.md.
