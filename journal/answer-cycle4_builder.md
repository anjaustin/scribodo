# Answer cycle 4, the register: the request builder in stages

**Date:** 2026-11-12. **Tree:** `ca908bd` plus the changes recorded below.
**Direction:** the P0 list in order; the register's "functions too large to
review" entry (AUDIT-003 F5), first item.

## The item

`build_in_worker` (`src/llm/chat.c`) built one turn's request in a single
300-line body with about 176 decision points: the preparation budget and a
locked copy of the session, the base request (edit or selected history), the
memory section (availability, the verified pipeline with the gate's arming,
recall and admission, the canon block, the trace), then sampling, remote
adaptation and the notices. Nothing tested a stage on its own; the register
asked for a split at the existing section comments into functions over a
shared context struct, each unit-tested.

## The change

The body is now eleven functions over two structs, in the order the comments
had. `ChatBuild` holds what every stage shares (budget, the history copy, the
context and names, the slot and base budgets, sampling, the token counter, the
request and the memory notice); `MemoryStage` holds what the memory stages
share (the bounded vector client, the plan, documents and attachment names,
the tokenizer and fingerprint, the database handle and project id,
availability, the history's oldest sequence, the query, and the recall marks).

| Stage | Function | Was |
|---|---|---|
| 1 | `build_begin` | the budget and the locked copy of the session, the slot and base budgets |
| 2 | `chat_build_base_request` (public, in `chat.h`) and `build_base` | the edit-length rule, the edit or selected request, the history error |
| 3 | `build_memory` | the memory section's guard |
| 3a | `memory_begin` | vectors, attachments, tokenizer, database, identity, scope, documents |
| 3b | `memory_pipeline`, `gate_arm` | the verified pipeline, its receipt, trace and notice; the gate's question, spans and author turns |
| 3c | `memory_recall` | the plan, the episode packing, the briefs, admission, the recall notices |
| 3d | `memory_canon` | the canon block |
| 3e | `memory_end` | the `MEMORY_PREPARE` trace, the plan and client freed, the budget ended |
| 4 | `build_finish` | sampling, remote adaptation, the request and notices into the session |
| | `build_in_worker` | the four calls |

Every expression moved as it was; only the names changed (`b->`, `m->`). Two
details are now explicit: `ChatBuild` and `MemoryStage` are zeroed before use
(the old locals `budget` and the memory section's `plan`, `documents` and
`attachment_names` were, respectively, uninitialised until begun and
zero-initialised; the tokenizer took the budget's address either way), and the
base stage is a plain function over copied state, so it has no session.
`ChatRequestHistory` gained a struct tag so `chat.h` can name it without
including `chat_history.h`, which includes `chat.h`.

The capture tools (`tools/answer_live_capture.py`,
`tools/answer_request_capture.py`) anchor on `server_worker` and `StreamCtx`,
which did not move; the live-capture build of this tree succeeded unchanged.

## Tests

`test_llm: build_base_request` (new): the base stage without a session. A
three-message history with the author's current request last, with and without
a document: the request carries the turns and the document text, nothing is
truncated, the first-history index is 0, the error buffer is untouched. An
edit: the passage and instruction, no history. A 20,000-byte passage with the
estimating counter: refused, nothing built, the error names the limit and the
token count. The other stages are reached by the existing fake-backend tests
(`fake_backend`, `fake_edit`, the memory tests in `test_memory_chat` with real
SQL admission) and by the live measurement below.

## Measurement

The product under its flags must build the same bytes. Two live runs on the
split tree's capture app (`build/answer-live-builder`), compared with the
previous tree's runs of the same scripts in the same environment
(`journal/answer-cycle4_relation.md`): unit ids and timings masked, score
pools as sets, candidate-pool counts masked in receipts (chat-turn ingest
variance, recorded there).

| | Baseline (previous tree) | Split tree | Requests identical | Replies identical | Receipts identical (unit ids and pool counts masked) |
|---|---|---|---|---|---|
| Second novel | `mars-relation` (352 s) | `mars-builder` (314 s) | **10 of 10** | **10 of 10** | **10 of 10** |
| First novel, index layer on | `pp-relation-index` (the relation tree, before the quotation-mark rule; 518 s) | `pp-builder-index` (475 s, gates running alongside) and `pp-builder-index2` (486 s, idle machine) | 5 of 10, both runs; the two split-tree runs agree with each other 10 of 10 | 5 of 10 | 6 of 10 |
| First novel, index layer on, the right baseline | `pp-base-index` (`ca908bd`, built in a worktree) | `pp-builder-index2` | **10 of 10** | **10 of 10** | **10 of 10** |

The first novel's first comparison was against the wrong baseline. The two
runs of the split tree agree with each other byte for byte, idle or loaded,
and diverge from `pp-relation-index` at the sixth question, where the second
verified span is two sentences with status `verified` rather than one with
`verified_prefix`: the quotation-mark rule of `ca908bd`
(`journal/answer-cycle4_relation.md`, "a sixth defect") verifies a span whose
quotation marks differ from the unit's, and the relation tree's app predates
that rule. The baseline for a builder change is the builder's parent commit,
so `ca908bd`'s capture app was built in a worktree and run idle; that row is
the measurement. The relation check's reaction to the new reply at that
question (a correct answer withheld, repaired in this step) is the relation
record's; the builder produced the same bytes as its parent for the same
inputs. The baseline app was built from `ca908bd` in a
worktree (`build/wt-ca908bd`, submodules checked out, the main tree's
third-party builds copied in) and ran idle (486 s): 10 of 10 requests, replies
and receipts identical to the split tree's idle run; the trace lines that
differ are the candidate-pool counts of two questions. The split produced its
parent's bytes on both novels.

The final tree of this step (the split and the relation repair) ran once more
on the first novel (`pp-final-index`) to confirm the repair live:
the first six requests and replies are byte-identical
to `pp-builder-index2` (the same reply at the sixth question, "fifteen"), and
that reply is now shown, `withheld=0`, `relations=0`, where the split run had
withheld it for "Bennet daughter is Lydia"; the four later requests differ
because the history now carries the shown reply rather than the withheld
notice, and so do their replies. Fixture: 25 passed, the same 2 source-delivery
checks failed as on every run of this script
(`build/product-memory-use/pp-final-index`, `build/builder/capture-pp-final-index`,
`build/builder/pp-final-compare.txt`).

## Gates

On the split alone (before the relation repair joined this step): `make test`
95,949 C checks and 168 Python tests, 0 failures (6 checks more than the
previous tree: the base-stage test); `make dbtest` 18,000, 0; `make analyze`
0 warnings across 88 source files; `make sanitize` 95,949, 0, 0 diagnostics;
`make tsan` 48,578, 0, 0 diagnostics (`build/builder/gates-*.log`). On the
final tree: `make test` 95,950 and 168, 0; `make dbtest` 18,000, 0;
`make analyze` 0 warnings across 88 files; `make sanitize` 95,950, 0, 0;
`make tsan` 48,579, 0, 0 (`build/builder/gates2-*.log`). Every exit status 0;
the tally sums every `N checks` line.

## Hosted verdicts

CI and Trusted acceptance both passed on `a98189a` (runs 43257408129 and
43257407732).

## What remains of the register entry

The observer's `run_job` and `observer_job_find` have the same shape and are
not split here; the script host's place (product binary or build flag) is the
author's decision. The entry stays open for those two.
