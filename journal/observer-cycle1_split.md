# Observer cycle 1: the two worker monoliths in stages

**Date:** 2026-11-12. **Tree:** `2fd2866` plus the changes recorded below.
**Direction:** "Let's wrap up the loose ends. 100/100. Methodically." The
loose end is TECHNICAL-DEBT's worker-monolith entry (AUDIT-003 F5): after
`build_in_worker` (`journal/answer-cycle4_builder.md`), `run_job` in
`src/llm/observer.c` (285 lines) and `observer_job_find` in
`src/llm/observer_job.c` (234 lines) remained. The yardstick is the headless
replay built in `journal/observer-cycle1_replay.md`: the observer with the
fake backend over the three test manuscripts with fixed canon, note and chat
seeds, byte-identical across three runs at the parent tree (717, 348 and 948
findings; `build/observer/base1-*`).

## run_job

One function owned extraction, the record lists, deduplication, the canon
judge with its lexical and semantic candidates, the note overlap, the
revision snapshot, the generation checks, the commit under the findings lock
with its revalidation, the semantic index update, the read marks and the state
save, and the cleanup. It is now fifteen lines over a `JobRun` (the job, the
fence, the candidates, the three record lists, the semantic key, the
conflicts, the outcome flags) and these stages, in the order the function had:

| stage | lines | does |
|---|---|---|
| `job_extract` | 8 | the backend reads the chunk; candidates |
| `job_load_records` | 11 | findings under the lock, canon, notes; `io_failed` when a list fails |
| `job_dedupe` | 18 | against stored findings and within the batch |
| `job_semantic` | 10 | the index key and the canon/notes sync |
| `job_canon_candidates` | 40 | the two nearest in meaning, then the best two by shared words, three at most |
| `job_judge_pair` | 20 | one statement against one entry; once per entry and passage; not when already recorded |
| `job_note_overlap` | 12 | the first live note sharing three words |
| `job_judge` | 17 | every non-question statement through the two above |
| `job_snapshot` | 14 | the revision snapshot when there is something to point at; the project unchanged |
| `job_revalidate` | 13 | under the write lock: the findings re-read, duplicates and recorded conflicts dropped |
| `job_create` | 24 | kept candidates and conflicts become records; stops at a generation change |
| `job_index` | 26 | documents as draft text, chats as chat; pruned points |
| `job_mark_read` | 23 | the piece hashes or the chat position into the state |
| `job_commit` | 12 | lock, revalidate, create, unlock, index, mark read, save; `ok` and `io_failed` as before |
| `job_end` | 11 | frees |

One helper is new in name only: `conflict_recorded` is the question the judge
stage and the revalidation both asked of a findings list in their own words
("a conflict for this canon entry with this text from this passage"); the two
inline loops were the same test. The `Conflict` type moved out of the
function. No condition, lock, generation check or order changed; the stage
comments carry the ones the function had.

## observer_job_find

One function listed the project's documents, pruned the state for documents
that are gone, read each document's paragraphs into pieces against the seen
hashes, built the "current pieces" list, then listed the chats, pruned deleted
ones, read each chat's position, copied its messages and filled the chunk. It
is now seven lines over `find_document_job` (49 lines: `list_documents` 37,
`prune_missing_documents` 7, `document_pieces` 52, `document_current_pieces`
13) and `find_chat_job` (53 lines: `chat_position` 11, `chat_pieces` 21). The
error paths are the function's: a directory or memory failure before any
document frees the names and reports the error with the job untouched; a
failure inside a document frees the job; a chat failure frees the job; a chat
whose unread messages are all failed, empty or rewrites is marked read and
the search goes on.

## Measured

`tools/observer_replay_run.sh` on the binary with the first split
(`build/observer/split1-*`) and on the binary with both
(`split2-*`), against the parent's baseline (`base1-*`):

| novel | baseline findings | after `run_job` split | after both splits |
|---|---|---|---|
| first (8 parts, 269 jobs) | 717 | identical | identical |
| second (28 chapters, 144 jobs) | 348 | identical | identical |
| third (34 chapters, 261 jobs) | 948 | identical | identical |

Byte for byte, with the seeded conflict, the note overlap and the chat
findings included. `test_llm` (`observer_run`, `observer_redteam`, 4,697
checks) and `test_components` (`observer_job_errors`, `observer_chat_filter`,
`observer_rules_and_refilter`, 104 checks) pass on each tree.

One compiler warning came from the split (a `const cJSON *` handed to a
helper that takes `cJSON *`); the qualifier was dropped on the parameter, the
helper's contract unchanged.

## What the split does not do

It does not shorten `worker` (161 lines: the wait loop, the rebuild branch,
the state load, the rename replay, the rules re-check, the job discovery and
the outcome bookkeeping), which AUDIT-003 did not name and which owns the
thread's synchronisation; and it does not add a unit test per stage, since
every stage takes the observer's private state. The product-level test of the
stages is the replay above and the two end-to-end tests. The script-host
decision, the entry's third item, is taken in `journal/debt-script_host.md`.

## Gates

`make test` 95,963 C checks and 174 Python tests, 0 failures (the same counts
as `2fd2866`: no test was added or removed); `make analyze` 0 warnings across
87 source files; `make sanitize` 95,963, 0, 0 diagnostics; `make tsan` 48,593,
0, 0 diagnostics; `make dbtest` 18,000, 0. Every exit status 0
(`build/observer/split-gates-*.log`, `split-gates-exit.txt`).
