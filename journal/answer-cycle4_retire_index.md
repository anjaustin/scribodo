# Answer cycle 4: the index layer retired

**Date:** 2026-11-12. **Tree:** `e258b76` plus the changes recorded below.
**Direction:** "Retire it." (the migration 015 question, put to the author
with three options: make the index layer permanent with an in-app resolver,
leave it as an experiment, or retire it).

## What was retired

The coreference index layer (`journal/answer-cycle3_coref.md`): a second text
beside every memory unit, the unit's text with pronouns resolved to names by an
external model (FCoref on torch, `tools/answer_coref_arm.py`, `venv-coref`),
loaded from a sidecar file named by `SCRIBODO_INDEX_TEXT_FILE`, stored in
`memory_units.index_text` (migration 015), and read in place of the text by the
lexical search, the embedder and the reranker. The original text was never
touched. The layer carried two first-novel passages the plain index missed,
changed candidate pools as well as their order, and was never measured on the
second or third novel. The product could not produce the sidecar, so the
experiment could never become a feature without a Python-and-torch dependency
at ingest.

## The move

MEMORY-015 §5: a concluded experiment's code is moved into the harness, not
deleted from the product without that move.

- `src/app/memory_index_text.{c,h}` are now `tests/memory_index_text.{c,h}`,
  harness code, with their test (`test_memory_index_text`, still in
  `make test`); the module reads the flag itself, since no product module may
  read an experiment flag. The arm, the environment lock and the recorded runs
  with the layer on (`pipe-coref1..3`, `pipe-window1..2`, `pp-relation-index`,
  `pp-builder-index`, `pp-builder-index2`, `pp-base-index`, `pp-final-index`,
  `pp-exact-index`) stay as the experiment's record.
- The product's plumbing for the layer is removed: the `index_text` field of
  `MemoryUnit`, its insert and hydration in the chat and document stores, the
  run-time `MATCH` column swap (now `u.text`), the embedder's substitution and
  validation, the reranker's scored text, the `ANSWER_INDEX_TEXT` trace line
  (the receipt contract header and the `answer_trace_pipeline` signature lose
  it; the trace fixture is regenerated), and the `index_text_file` field of
  `Experiments` with its `getenv` (ten fields remain).
- Migration 016 drops the FULLTEXT key and then the column, in two statements
  each re-runnable (`IF EXISTS`), so a death between them recovers; the schema
  version is 16. The column never held author text (NULL, or a derived copy of
  the text under the experiment), so nothing of the author's record is deleted.

## Measurement

**A defect of my own, caught by the gates.** The first tree of this step
removed every site that named `index_text` and left the chat store's hydration
asserting eleven result columns where ten now came back (`r.ncols == 11`), a
check the column's name does not appear in. `make test` passed (no database);
`make dbtest` failed 21 checks, every one a unit that would not hydrate, so no
unit reached the outbox; `make crashtest` failed 98 because the receipt check
still counted fifteen; and the two live runs built on that tree never finished
ingesting (`wait_memory` timed out, no candidate hydrated, 7 and 5 fixture
failures). Those runs are kept as `*-retire-broken`
(`build/product-memory-use`, `build/retire`) and are not a measurement of
anything but the defect. The count is now ten, the receipt check sixteen, and
every gate and both runs were redone on the fixed tree. The lesson is in the
memory notes: a column removal is complete when the column-count assertions and
the schema-version pins change with it, not when its name is gone.

On the fixed tree: `make test` 95,955 C checks and 168 Python tests, 0
failures; `make dbtest` 18,000, 0 (schema 16 on every throwaway database);
`make crashtest` 1,440 checks, 0 failures, 129 matrix rows, 125 deaths (the
migration rows now kill after each of 82 statements and 16 receipts, and
recover to schema 16 with the clean fingerprint); `make analyze` 0 warnings
across 87 source files (one fewer: the module left the product); `make
sanitize` 95,955, 0, 0 diagnostics; `make tsan` 48,585, 0, 0 diagnostics.
Every exit status 0 (`build/retire/gates2-*.log`; the tally sums every
`N checks` line).

**Live.** The product's default path never read the column, so its bytes must
not change. The second novel on this tree's capture app (`mars-retire`, 329 s)
against the parent tree's run (`mars-exact`): **10 of 10 requests, 10 of 10
replies and 10 of 10 receipts identical** (unit ids and pool counts masked);
the trace lines that differ are the candidate-pool counts of two questions.
The first novel ran without the layer for the first time since the window
cycle (`pp-retire`, 442 s): 23 of 27 fixture expectations passed, the same
four failures as `pp-relation`, the last run without the layer: the two
source-delivery checks every run of this script fails and the two passages
(the entail and the fortunes receipts) the layer had carried. That is the
reach the retirement gives up, measured: two designated passages on the first
novel, none on the second (the layer was never on for it). `pp-retire` is the
default-path baseline for the first novel from here.

## Records

TECHNICAL-DEBT (the flag-lifecycle entry, closed), AUDIT-003 (F4 note),
TESTING, LANDSCAPE row 12, `journal/answer-cycle3_coref.md` (decision 4), the
GAP-004 matrix (the migration rows' counts).
