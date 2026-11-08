# MEMORY-016 — Deterministic recall view (S1)

**2026-11-07. Implementation record for [MEMORY-015](MEMORY-015-effective-memory-contract.md) slice S1.**

**Status:** implemented and validated on this Mac. The Recall panel is in the
product and needs no flag. It is the first authoritative surface of the
canon-first direction in [MEMORY-014](MEMORY-014-canon-first-continuity.md):
host-rendered, exact, bounded, and with no model anywhere on its path. This
record makes no quality claim about chat continuity, enables no memory by
default, and changes no prompt, tier, consent or model setting.

## What it does

The author types a query in the **Recall** panel and presses Enter. A
background job reads the project store and returns:

- every **committed canon entry** in the project whose current wording
  contains at least one query term, with its exact current text and version;
- **verified original excerpts** from conversations whose memory the author
  has enabled: chat messages, and saved document revisions ingested under
  those conversations, each re-checked byte-for-byte against its source;
- a **coverage line**: how many committed canon entries were searched and how
  many retracted ones were excluded; how many conversations the project has,
  how many had memory on, off, or still indexing; how many excerpt candidates
  were considered and how many were rejected because their source had changed;
  and any limits that bounded the search.

Clicking a canon row opens that entry's panel. Clicking an excerpt selects its
exact source passage through the same `open_source` path findings and canon
use. Nothing is written anywhere by this surface.

## Design decisions

| Decision | Reason |
|---|---|
| **Lexical matching only.** Query terms are split on ASCII whitespace and punctuation, ASCII-lowercased, deduplicated, and matched as case-insensitive substrings. | MEMORY-014 §8.1 requires no inference call on the authoritative path. The embedding model is an inference call, and `semantic_canon_for` cannot distinguish "no match" from "the embedder failed". Deterministic matching is inspectable and has no failure mode that looks like absence. |
| **Canon is project-wide; excerpts respect per-conversation consent.** | Decision of 2026-11-07 in MEMORY-015 §5. The record of the book is the record of the book. Memory consent is granted per conversation and the view never widens it; the coverage line counts the conversations it could not search. |
| **Excerpts are re-verified at display time.** A chat excerpt is admitted only if the planner's SQL eligibility holds, its message's content hash is unchanged, and its bytes still equal the message at the recorded range. A document excerpt must equal the revision's bytes at its range and carry matching hashes. | MEMORY-014 §8.1 "Provenance: resolves or the result is rejected". Rejections are counted, not hidden. |
| **Fixed ranking.** Canon first, by distinct terms matched, then newest version, then id. Excerpts by terms matched, then the SQL full-text score, then kind, then unit id. | No tuning knob, no learned ranker, stable between runs on the same data. |
| **Bounds.** Query ≤ 512 bytes, ≤ 16 terms; ≤ 64 conversations searched (the most recently updated); ≤ 16 excerpt candidates per source kind per conversation; ≤ 32 canon rows and ≤ 64 rows in all. Every bound that bites is named in the coverage line. | Cost axis of §8.1 and the standing rule that absence is stated as bounded coverage. |
| **Off the frame thread.** `RecallJob` runs `recall_run` on an owned worker with its own store connection and hands over one result per run. A result for a project that is no longer open is dropped. | Same pattern as the entity backfill and name detection jobs. |
| **Failed-experiment code stays archived in place.** The compact and episode presentations are untouched. | "Delete nothing, only archive." They are idle-only, flag-gated and exercised by the native eval harness. |

## Code

- `src/core/recall_terms.{h,c}`: term extraction and matching, pure.
- `src/app/recall.{h,c}`: `recall_run`, `RecallResult`, `recall_result_text`.
- `src/app/recall_job.{h,c}`: the owned background job.
- `src/ui/recall_view.{h,c}`: the panel, its query editor, coverage text and rows.
- `src/main.c`: the toolbar button, the right-dock panel (shares the dock and
  width with the observer; opening one hides the other), focus, row actions,
  per-frame start/poll, script hooks.
- `src/app/script.{h,c}`: `wait_recall`, `expect recall`, `expect recall_not`.

## Gates (MEMORY-014 §8.1) and evidence

| Axis | Pass condition | Evidence |
|---|---|---|
| Exactness | Displayed canon text is byte-equal to the current committed version | `test_recall: coverage and exactness` compares `row->text` to the committed string with `strcmp`; the UI script checks the committed wording appears verbatim |
| Provenance | Every excerpt resolves at its recorded range or is rejected | `test_recall: provenance rejection` changes an original message and a saved revision and checks the excerpt disappears and the rejection is counted; restoring the original brings it back |
| Status | Only committed canon is authoritative | Retracted canon is excluded and counted; `tools/test_recall_mutations.py status_filter` kills the weakened check |
| Coverage | Counts are accurate; absence is bounded | Exact counts asserted for committed, retracted, conversations, memory on/off/indexing, candidates, rejected; a no-match query reports `rows=0` with its counts in the UI script |
| Scope | Cross-project, revoked consent, paused store fail closed | Another project's matching canon is never returned; disabling a conversation's memory removes its excerpts while project-wide canon stays; a file-backed project reports that excerpts need the store |
| Cost | Within existing bounds | Query, term, conversation, candidate and row caps are enforced and named; `bounds` seeds 70 matching canon entries and sees 32 |
| No-model | No inference call on the authoritative path | By construction: `recall.c` includes no `llm/semantic.h`, `llm/server.h` or `llm/memory_brief.h`, and `test_recall` creates no semantic client and starts no model server |
| False authority | Zero on this surface | Every row is either the current committed canon text or verified original bytes; there is no generated text in the panel |

## Validation

Run on this Mac on 2026-11-07, pinned services, no model server involved:

| Suite | Result |
|---|---|
| `make test` (includes `test_core: recall_terms`) | pass, 0 failures |
| `make sanitize` | pass, 0 failures |
| `make dbtest` (includes `test_recall`, 381 checks) | pass, 0 failures |
| `make tsan` | pass, 0 failures |
| `tests/ui/store/15_recall.scribo` (in `make uitest-db`) | 33 passed, 0 failed |
| `python3 tools/test_recall_mutations.py` | control passes; 6 of 6 mutants killed (status filter, canon term match, canon rank, canon cap, no-terms guard, query bound) |

## Limits, stated plainly

- **Asymmetric candidate selection for excerpts.** Canon is matched host-side
  by substring, so any term of two or more bytes can match. Excerpt candidates
  come from MariaDB's InnoDB full-text search in natural-language mode at the
  server's defaults: tokens shorter than three characters and words on the
  built-in stopword list do not select candidates. A query of "17" or "the"
  will match canon but select no excerpts. The host then re-checks every
  candidate by substring, so a shown excerpt always contains a term, but an
  excerpt that only a short or stopword term would reach is not shown.
- **Case folding is ASCII only.** "Zoë" matches "zoë" but not "ZOË".
- **Substring, not word, matching.** "key" matches "keys" and "monkey".
- **Memory excerpts need the project store** and a conversation whose memory
  the author enabled. File-backed and paused projects show canon only and say so.
- **The 64 most recently updated conversations** are searched. Older ones are
  not, and the coverage line says so when the cap bites.
- **This is a lookup surface, not continuity.** It shows what the author
  stated and what was said. It does not infer decisions, does not reach the
  model, and is not evidence that chat answers will use canon correctly. That
  is MEMORY-014 P1, which has its own gate.

## What is next

S2 in MEMORY-015: a realistic manuscript-scale fixture and the two-metric
harness (did the right source reach the model; did the model use it
faithfully), so that selection work in S3 is measured against something larger
than toy facts. Nothing in this record authorizes P1 or changes defaults.
