# Memory MVP — implementation progress

**Date:** 2026-11-07 (latest pipeline checkpoint)

**Baseline:** [MEMORY-002](MEMORY-002-mvp-design.md). Implementation authorized
after the design pass. This is a partial foundation, not the complete memory MVP.

## Implemented: bounded request history and source traversal

- `src/llm/chat_history.c`: an owned request-history snapshot with hard limits of
  64 messages, 64 KiB content and 256 inspected messages. It selects recent
  eligible turns before allocation/copying, excludes failed replies and excludes
  local turns from remote requests. It drops an orphaned oldest assistant turn.
- `src/llm/chat.c`: the worker uses this snapshot, not a copy of the complete
  conversation. Reviewed edits copy **no** chat history. Per-session copied-byte,
  selected-message, inspected-message and limiting counters are available through
  `chat_history_stats()`. Normal model token budgeting still follows selection;
  these are host-work limits, not a replacement for tokenizer/template accounting.
- An oversized current chat request (>64 KiB) fails preparation with an explicit
  error, rather than being shortened. Optional older messages are omitted as a
  recent contiguous selection boundary, with a visible notice. This cap applies
  to local and remote chat request snapshots; no new remote retrieval is added.
- `src/app/store_chat_history.c`: database-only source traversal using indexed
  sequence seeks and bounded binary substrings. Pages carry at most 64 chunks /
  64 KiB source text; each chunk is at most 8 KiB, with exact source message ID,
  sequence, UTF-8 byte range, length, role and stored flags. Large originals are
  split, not truncated or fully hydrated in the client.
- The cursor captures an append high-water mark on its first page. Later appends
  cannot extend that traversal. Sequence gaps, empty bodies and UTF-8 boundaries
  are supported. A failed read returns no partial page and does not change the
  caller's cursor. The same-project SQL join is applied on every read.
- Source traversal uses one-shot `ProjectStore` routing; borrowed database
  connections never escape. File-backed and paused projects fail explicitly,
  without a whole-file/cached-vector fallback. Original persistence remains intact.

No schema migration, memory job, vector-index write, summarization or model change
is introduced in this foundation. Source chunks deliberately do **not** claim
historical attachment revisions or canonical/approved facts.

## Evidence

Reproducible fixtures live in `tests/test_llm.c`, `tests/test_db.c` and
`tests/test_persistence_faults.c`.

| Fixture | Observed result |
|---|---|
| 101 / 1,001 / 10,001 / 100,001 in-memory archive messages, 12-byte text | Each copied 63 messages / 756 content bytes and inspected 64 messages |
| Sparse remote history in a large local archive | At most 256 messages inspected; local text never copied into the snapshot |
| Fake worker with 10,000 prior messages | 63 messages prepared; archive untouched; subsequent edit prepared zero history |
| Real MariaDB, 200,003-byte Unicode message + empty failed remote reply + 130 small messages | Exact reconstruction across six bounded pages; append after page one excluded |
| Wrong project, invalid continuation, malformed stored timestamp, sequence gap | Wrong project yields no sources; invalid reads fail empty; keyset traversal crosses gaps |
| Single and persistent allocation failures at every snapshot allocation | Failure returns empty owned output; cleanup is sanitizer-checked |

Validation commands:

```sh
make -j8 all test
make dbtest
make sanitize
make BUILD=build/sanitize OPT='-O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined' dbtest
make tsan
make uitest
```

These suites passed during this implementation pass. Logs are under
`build/memory-*.log`. Synthetic materialization counts are not latency, RSS,
whole-manuscript recall, model fidelity or a sustained-use SLA.

## Remaining before slice 1 is complete

**Update:** the DB UI windowing and source-identity portions of items 1–2 below
are now implemented in [MEMORY-004](MEMORY-004-history-windows.md). The list is
retained as the original checkpoint; full-reader compatibility consumers and
the workload/performance gates are still pending.

The follow-up [REDTEAM-015](REDTEAM-015-history-windows.md) reproduced and fixed
presentation/action freshness, retained allocation capacity, empty navigation,
unsaved DB chat lifecycle and stopped-reply persistence. The latter introduces
migration 008; the no-schema-change statement above describes only this original
request-history/source-traversal checkpoint.

The next [MEMORY-005](MEMORY-005-async-history.md) slice adds owned asynchronous
DB navigation/source reveals, publication identity/epoch fences, connection-lock
remediation and repeatable percentile/resource diagnostics. It does not complete
the sustained-use gates or add ingestion, Qdrant memory projections or summaries.

[MEMORY-006](MEMORY-006-retrieval-pipeline.md) adds opt-in durable chat ingestion,
fenced index outbox/projections, SQL-validated retrieval-only planning and as-of
reply sources. It remains chat-only, experimental and default-off; document
revision retrieval and generated T1/T2 briefs are still unfinished.

[MEMORY-007](MEMORY-007-saved-revision-authority.md) adds a separate saved-revision
receipt and bounded disk verification, so unsaved AI/restore/source snapshots
cannot masquerade as saved attachment authority. It is a prerequisite only;
document-memory ingestion/retrieval and attachment-set fences remain pending.

[MEMORY-008](MEMORY-008-saved-document-retrieval.md) implements scope-local saved
revision ingestion and attachment-filtered recall, including detach/save/dirty
admission fences and typed Sources. It remains default-off; T1/shared generative
admission, optional measured T2, cleanup/deadlines and quality gates are pending.

1. Windowed/paged `ChatSession` and UI viewport/cache integration. `open_chat()`
   still loads the complete conversation and the UI still retains archive-sized
  state. Do not declare host memory bounded end-to-end yet.
2. Stable source-ID/sequence mapping throughout source navigation, finding
   reveals, record creation and persistence callbacks before evicting any visible
   history. Never substitute window array offsets for durable source sequences.
3. Baseline and post-change UI/input latency, actual process/service memory,
   prompt-preparation timing and contention measurements on representative
   manuscript fixtures and an hours-long soak.

The source pager is ready for ingestion consumers, but no UI or observer consumer
has been silently switched to partial history. Existing full-load APIs remain for
export/import/observer compatibility until their consumers are migrated safely.

## Subsequent slices

The chat-only scope/epoch authority, SQL outbox/leases, immutable Qdrant
projections, source-validated retrieval-only planning and reply provenance are
implemented in MEMORY-006. Next: saved-document-revision eligibility/retrieval,
T1 briefs with shared generative admission, then optional measured T2 comparisons
and whole-manuscript quality/resource gates. The pager is append-stable, not a transaction
snapshot against historical edits. SQL output limits also do not prove bounded
database-server I/O or constant seek latency for large LONGTEXT sources.

## Red-team follow-up

[REDTEAM-014](REDTEAM-014-memory-foundation.md) records the adversarial findings,
red/green regressions, mutation checks and final validation. Remediations make
chat loading and send preparation atomic on allocation failure, preserve loaded
metadata, reject missing mandatory requests/provider markers, combine document
and history-limit notices, and replace the initial aggregate high-water scan
with a reverse indexed seek. Invalid terminal cursor states are rejected.

The standalone history suite includes 1,000 deterministic randomized fixtures;
`python3 tools/test_memory_mutations.py` confirms five weakened selectors are
detected while the unmodified control passes. These checks do not expand the
implemented feature scope or establish real-model quality/performance claims.
Existing real-model adapter and UI compatibility suites also pass against the
pinned local BF16 model; no new recall benchmark or hierarchy experiment was run.
