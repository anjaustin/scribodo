# Memory foundation — asynchronous history and scale diagnostics

**Priority:** GAP-003 / P0. **Status:** asynchronous DB chat/source navigation is
implemented; this is not completed ingestion, retrieval or sustained-use approval.
Extends [MEMORY-004](MEMORY-004-history-windows.md).

## Author-visible behavior

- Opening a stored chat, Older/Newer/Latest, and opening a chat source read on an
  owned background worker. The existing transcript and composer remain available
  until an accepted successful replacement; failed/cancelled reads keep them.
- One worker, one pending request and one completed result are retained. New
  requests supersede old intent, including completed-but-unconsumed results.
  There is no thread or request allocation per navigation click.
- The Send button becomes **Cancel** while a replacement is pending. Escape in
  the chat also cancels that intent immediately. Cancellation invalidates output;
  it does **not** interrupt a blocking database/network call. The next request
  waits for that call to finish, and shutdown joins the worker before the store.
- Enter while browsing an older window loads Latest and keeps the composer.
  Once ready, the author reviews attachments/provider and presses Send again.
  There is no delayed automatic send that could use changed prose or consent.
- Opening a different chat binds its identity only after successful replacement.
  Source navigation submits one focused request, not competing Latest/Focus
  requests. Durable sequence/codepoint offsets remain distinct from array indices.
- Attachment edits made while loading are preserved. A monotonic attachment
  revision detects detach/reattach ABA changes, not merely equal final strings.

## Publication and connection ownership

`ChatHistoryJob` copies the project/conversation/request and uses an internal
nonwrapping generation, independently of caller tickets. A caller owns a polled
page; rejected, superseded, partial failed and unconsumed pages are freed.
The worker closes its own thread-local store connection on exit.

Main-thread publication requires matching intent, project session generation,
previous conversation identity, transcript version, idle/saved chat, online
store, store lifecycle generation, immutable project ID and actual folder
device/inode. New Chat/project changes/recovery invalidate pending intent.
Folder replacement/rebinding at the same path and store restart are therefore
not mistaken for the original request's authority. Local identity snapshots do
not open a connection or query SQL; the worker still uses ProjectStore's actual
same-project authorization. These snapshots are not a transaction against an
external adversary repeatedly rewriting files/database rows.

Worker DB connection establishment no longer holds the manager lifecycle mutex
across network I/O. A configuration snapshot is checked against the current
generation afterward; stale successes are closed by their owner and stale
failures cannot take a newer store offline.
Verification-cache hits also check the actual folder device/inode and marker's
claimed identity; a copied folder cannot inherit a warmed success merely by
occupying the old path. DB-only path edits with unchanged identities remain
subject to the existing 30-second verification TTL.

Navigation chooses the worker from the local store marker instead of doing a
foreground SQL inspection first. Passive capability/list/finding refreshes are
deferred while a history read is active, including cancelled reads still waiting
on I/O. These are presentation snapshots, never new write authorization.

## Adversarial findings and evidence

- A gated connection test reproduced the old manager-lock stall. Fourteen
  connect/stop/restart/offline cases now cover ownership and stale success/failure.
- Review found path-only publication insufficient across store restarts and
  folder replacement. Local identity and epoch checks were added. A UI mutant
  removing only the store-generation check failed the restart regression: Older
  incorrectly replaced Latest (one failure, other store UI fixtures passed).
- A new local identity probe initially blocked on a FIFO marker. The red test
  terminated with SIGALRM. Marker reads now open nonblocking and accept only an
  opened regular file. This is not a general filesystem wall-clock guarantee.
- The follow-up malformed-marker test reproduced an unsafe fallback to file
  storage (SIGABRT assertion). Existing but unreadable/malformed/nonregular
  markers now pause structured storage instead of creating a parallel history.
- Worker mutation review exposed missing test cases, not another implementation
  failure: cancellation without a newer request, and a transient stale result
  before its replacement finished. Deterministic gates now cover both.
- Final review also required a warmed-worker folder-replacement regression and
  successful restoring publication after attachment edits (including ABA),
  rather than accidentally superseding that restore before testing its guard.
  The replacement test reproduced three real-DB failures before the cache-key
  fix. The revised attachment test detects removal of the main-thread attachment
  revision check (eight failed expectations, including cascades; other store UI
   fixtures passed). The revised unmodified UI control subsequently passed all
   47 checks, including successful attachment-preserving publication and ABA.
- `python3 tools/test_history_worker_mutations.py` passes two controls and kills
  five weakened worker/store protocols. Compile failures are errors, not kills.
- The scripted UI gate holds a fetched result before publication. The suite
  checks composition, blocked sends, New Chat, project A/B/A, Escape/button
  cancellation, attachment changes and restart fencing. `wait_history` waits for
  application/rejection and worker settlement, not just thread completion.

## Repeatable DB workload diagnostic

```sh
make historybench                # 15 samples per case
make historysoak                 # 500 samples per case
tools/history_bench.sh soak --iterations 2000
```

The tool builds a separate DB-only client and creates/drops an exactly named
disposable database. It does not start/stop services or use a manuscript project.
Seed sizes are 100, 1,000 and 10,000 synthetic Unicode/prose messages (64–8,192
bytes), with one 262,147-byte source in each archive. Gates verify returned
row/body bounds, UTF-8/content and exact oversized-source reconstruction.
Bad options/ports/database names and service failures fail nonzero; cleanup
failures report the exact disposable database and retry command.

One 500-sample-per-case run against local MariaDB 11.4.13, concurrently with
validation work, produced the following 10,000-message results. The archive held
27,526,083 body bytes. Three warmups preceded each case.

| API | p50 ms | p95 ms | p99 ms | Rows | Returned body bytes |
|---|---:|---:|---:|---:|---:|
| Latest | 5.444 | 6.644 | 8.211 | 58 | 65,535 |
| Older | 5.567 | 7.463 | 9.376 | 57 | 65,536 |
| Focus | 5.399 | 7.829 | 9.034 | 57 | 65,536 |
| Source page | 5.391 | 6.578 | 7.688 | 8 | 65,533 |

Whole-client cumulative RSS high-water was 8.50 MiB (includes migration/seed;
not per-API isolation or server RSS). Timing includes the warm DB call and client
allocation but excludes seed/validation/free. This is a short repeated workload,
**not an hours-long soak**, cold-start gate, frame/input latency, model recall or
service-resource SLA. Concurrent tests make it unsuitable as a controlled
before/after performance comparison.

## Validation and remaining gates

Local validation includes `make all test dbtest`, ASan/UBSan (including DB),
`make tsan`, ordinary/store/real-model UI acceptance, local model compatibility,
the existing selector mutations and the new worker/store mutations. Logs remain
under `build/async-history-*.log`, not in version control.

**Final-rerun status (2026-11-07):** headless/real-DB checks and ASan/UBSan passed
after the verification-cache fix (11,250 DB checks plus 43 schema checks). The
final TSan rerun passed. After a temporary inability to create a macOS window,
the ordinary/store/real-model UI, model compatibility and backup/restore rerun
also passed, including all 47 revised async-history UI checks. Evidence is in
`build/async-history-retry-acceptance.log`. Earlier command/watchdog interruptions
and unavailable-display runs are diagnostic evidence, not test passes. Hosted
acceptance is checked separately for the pushed commit.

Remaining P0 foundations include safe in-flight I/O interruption/request-wide
deadlines, measured frame/input percentiles under real service stalls, cold/warm
whole-process/service memory and hours-long sustained use. Store startup,
migration, mutations, several other UI queries and full-history compatibility
readers remain synchronous. This change must not be described as making all UI
or shutdown paths outage-responsive.

The next [MEMORY-006](MEMORY-006-retrieval-pipeline.md) slice adds opt-in chat
ingestion/receipts/outbox, immutable Qdrant projections and SQL-validated
retrieval-only planning. T1 briefs, optional measured T2 checkpoints and the
remaining foundation gates are still pending. No local memory is exported to
remote providers by these slices. The other four product gaps remain deferred.
