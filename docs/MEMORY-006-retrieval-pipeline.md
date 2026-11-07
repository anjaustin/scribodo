# Memory MVP - durable chat ingestion and retrieval-only experiment

**Priority:** GAP-003 / P0. **Status:** experimental, chat-only, default off. This
extends [MEMORY-005](MEMORY-005-async-history.md), not a declaration that the
whole-manuscript memory MVP or its sustained-use gates are complete.

## Trying the experiment

Start the existing local services, then launch with
`SCRIBODO_MEMORY_EXPERIMENT=1`. Open a stored conversation and right-click it:
**Enable local memory**, **Disable local memory**, and **Rebuild local memory**
are explicit per-conversation controls. Setting the environment flag alone does
not grant consent. File-backed/paused projects never fall back to a second
memory history. Only the current selected conversation is processed by the
owned worker; other conversations are not automatically retrieved.

The memory status row deliberately describes partial/idle/backlog states, not
exhaustive recall. Sources outside the recent window can now contribute bounded
local reference excerpts. This is not a replacement for reading an entire book.
Saved attachment revisions, notes, canon and observer findings are **not** new
automatic retrieval sources in this slice. Existing explicit attachments retain
their original behavior and truncation notices.

Local memory is bypassed for remote requests and reviewed edits, before any
memory SQL/candidate/embedding access. There is no automatic local-to-remote
fallback. Normal provider selection, explicit-attachment consent and author
review remain unchanged.

## Durable authority and ingestion

Migration **009** adds scopes, immutable units, per-source completion receipts,
and an index outbox, with composite same-project/conversation foreign keys.
Scope enablement/policy/history/index generations fence old work. New message
commits update durable ingestion intent in the **same transaction**; enabling
also records the indexed latest sequence as a backfill high-water mark.

Ingestion claims a DB-clock lease with a monotonic fence, reads at most 2,003
source bytes, and publishes at most one bounded unit/classification per step.
Units contain at most **2,000 UTF-8 bytes and 512 measured source tokens** so the
pinned embedding adapter never silently clips them. Tokenization happens outside
transactions. Unknown counts fail closed; tokenizer/protocol identity is pinned
to a generation and checked by ingestion, hydration and foreground planning.
A changed identity requires rebuilding rather than reusing old token counts.

Receipts become complete only after all source chunks are classified/published.
The cursor advances through the next source, including sequence gaps; it is not
`MAX(completed)`. Remote sources and failed/stopped/truncated assistant replies
are classified without generating units. Local author requests survive failed
turns, including stopped/error-marked user metadata. Roles are checked explicitly.
Historical stopped flags lost by old versions still cannot be recovered.

Unit publication, source receipt, cursor progress and index intent commit
atomically. Outbox acknowledgment requires the current consent/epochs/generation,
owner, fence, unexpired lease and source version. Failures back off, with at most
three attempts before explicit intervention/rebuild. No model/tokenizer/Qdrant
call occurs while holding a memory publication transaction.

`messages.source_hash` is generated/persisted once when content is written or
changed. Migration backfills it once, linearly over existing source bytes. This
avoids rehashing a whole LONGTEXT for every small chunk/candidate/acknowledgment.
It does **not** establish constant server-side storage/substr work: large-source
SQL execution/I/O and migration wall time still need workload measurements.

## Qdrant is a locator projection

Collections are isolated by originating SQL database, project, memory schema,
embedding fingerprint and index generation. Fake/test embeddings have a separate
fingerprint. Database origin is copied from the SQL connection; a manager restart
cannot redirect an old source lease into the new database's namespace.

Point IDs derive from immutable source artifact/hash, scope/epochs, embedding
fingerprint and generation. Payloads contain **locators, not source text**.
Completed writes use `wait=true` and validate the response receipt. Retries target
the same point. Conflicting duplicates, wrong scope/fingerprint/generation,
malformed hashes, escaped NULs and mixed poisoned results fail closed.

A missing collection causes SQL generation/ingestion reseeding before creating
its replacement. Rebuilds are bounded per ingestion step; they do not load or
enqueue the archive in one client operation. Existing originals and retired
derived generations are retained. Automatic retired-generation cleanup is not
implemented; repeated rebuilds increase disk/index use. Admission uses an explicit
active-generation/state index; an EXPLAIN fixture includes 4,096 retired jobs.

## Retrieval and dispatch admission

The planner recalls at most **16 lexical + 16 vector locators**, selects at most
**eight original excerpts**, and caps the rendered optional reference at **16 KiB
and 4,096 measured tokens**, including neutralization/framing. Ranking deduplicates
identities; one recall channel cannot inflate rank through repeated locators.
The recent protected tail is excluded by committed sequence.

Every candidate is hydrated from SQL after consent, scope/database, epochs,
generation, source hash, role/eligibility and tokenizer checks. Qdrant text is
never a fallback. Source/body edits make old locators ineligible; unsupported
historical mutations during partial ingestion require an explicit rebuild.

Only complete optional units are dropped when fitting the final request. The
author's current words and explicit document block are not cut to make room for
memory. The pinned local server's `/apply-template` and `/tokenize` count the
complete request and generation framing; unknown counts refuse memory admission.
SQL scope and local folder/store identity are rechecked after counting, before
dispatch. This is an as-of admission point, not a promise to retract bytes already
processed after later policy/source changes.

Requests reject trailing JSON, duplicate keys, escaped NULs and malformed UTF-8
rather than parsing/reprinting a shortened author instruction. Source references
are reference data, not privileged instructions, canon or independent evidence
of truth. Lexical FULLTEXT still has stopword/short-name/language limitations;
an empty top-k result does not prove absence.

## Controls, ownership and reply provenance

The persistent `MemoryJob` owns one worker, bounded control/selection state and
its thread-local SQL connection. Resource pauses yield between steps, not during
arbitrary blocking I/O. Accepted consent controls must not be replaced by a
navigation resume; sends wait for pending controls to commit. Pending settings
also participate in close/discard protection. They are not durable until SQL
commit, and explicit discard can abandon uncommitted controls.
Rebuild on an absent/disabled scope is an acknowledged no-op: it never enables
memory, changes that scope's generation, or blocks a later Enable command.
Committed controls update the visible consent status even while resource work
is paused. Cold local model startup consumes no ingestion/index retry attempts.

Each admitted local reply records a bounded **as-of prepared source snapshot**
alongside its normal message. The snapshot survives DB/file compatibility
load/import/export and window paging. **Sources** opens a read-only scratch view;
it cannot create a note/canon record or edit prose. It describes context admitted
for that reply, not proof of server processing, current eligibility or truth.
Failure before final request preparation clears the pending snapshot.

Provenance is never copied into model request history, including remote history.
A DB display window can additionally retain at most 64 x 16 KiB of provenance
metadata, separate from its existing 64 KiB body budget. Worker/chat dependencies
are joined before destroying the semantic client, store or model servers.

## Evidence and remaining scope

Tests cover SQL rollback/outbox atomicity, partial/restarted ingestion, two-owner
leases, stale/expired fences, backoff/exhaustion/rebuild, role/remote classification,
tokenizer changes, source mutation, duplicate poisoning, strict request parsing,
allocation sweeps, remote bypass, provenance round trips and pointer presentation.
See `build/memory-*.log` for local generated evidence, not committed fixtures.

Final local validation after the disabled-Rebuild/cutoff-warning fixes passed:
`make -j8 all test dbtest`, `make sanitize TEST_TIMEOUT=600`, sanitizer-backed
`dbtest`, `make tsan`, and `make uitest uitest-db llmtest uitest-real backuptest`.
Clang static analysis of the store, worker, planner, tokenizer, chat and history
window modules reported no diagnostics. The pipeline mutation suite passed two
unmodified controls and killed eleven weakened protocols; this is targeted test
sensitivity, not a claim of complete fault coverage. Document cutoff notices
remain first when appending bounded memory status/source information.

Post-push trusted database/UI acceptance passed. Hosted CI exposed a test-only
scheduler assumption in the consent FIFO test: 100 short sleeps can legitimately
span more than the one-second retry backoff on a loaded runner. The regression
now checks the retry deadline at callback entry rather than assuming a loop's
maximum elapsed time; FIFO order, bounded controls and paused-resource assertions
remain intact. A mutation that bypasses consent retry backoff is detected.
File compatibility tests also verify that escaped-NUL provenance/provider fields
are rejected by the existing whole-file guard, not silently shortened on import.

The store UI fixture retrieves an author-established fact at sequence 0 after it
has left a 130-message conversation's 64-message window. It also verifies explicit
consent, disable behavior and source reopening. A separate real-model oracle-source
control answered **amber** and matched **131 final prompt tokens** against the
server's usage receipt. That control does not establish ANN recall quality or
whole-manuscript answer fidelity; the UI's fake model checks admitted sources,
not language-model reasoning.

Frame diagnostics report actual interframe distributions (including pacing), not
OS input latency or isolated CPU time. Short held-history runs are not hours-long
real-model use. Sustained pipeline/frame/RSS tools remain explicit diagnostics;
their measured durations and backends must be reported, not inferred from names.

A final-code short headless run sustained 30.001 seconds (52.344 seconds total,
including warmup/recovery), with real local MariaDB/Qdrant and fake embedding/
byte-token backends. It appended 30 turns, ran 112 successful source-recall/apply
plans and recovered once from collection loss in 18.805 seconds. Source append
p95/p99 were 11.375/13.918 ms; planner p95/p99 were 33.000/35.500 ms. Sampled
process RSS rose from 12,206,080 to 14,991,360 bytes (also the sampled peak).
There were zero bad source hashes or unit hash/range checks; the final ingestion
and index backlog drained. Service RSS, real inference and model quality were
not measured by this run. These are one local workload's observations, not SLAs.

The completed one-hour headless run sustained **3,600.023 seconds** (3,618.353
seconds total), using real local MariaDB/Qdrant with fake embeddings and byte
token counts, without UI or inference. It appended 3,492 turns, completed 12,267
source-recall/apply plans, exercised 713 pause/reselections and recovered once
from collection loss in 20.679 seconds. Final ingestion/index/control backlogs
were empty; 7,117 source rows and 8,356 derived units had zero bad source hashes
or unit hash/range checks. Source append p95/p99 were 15.250/19.000 ms; planner
p95/p99 were 47.000/53.500 ms. Sampled process RSS was initially 12,058,624 bytes,
peaked at 14,925,824, and ended at 7,962,624. The disposable database was cleaned.
This run used the binary built when launched, before the final cold-start,
disabled-Rebuild and cutoff-warning hardening; the final-code short run and
regressions above cover those later changes. Service RSS, real-model fidelity,
whole-manuscript quality and OS input latency remain unmeasured by this workload.

The completed held-history UI/frame soak measured **3,875.811 seconds** across
225,900 frame intervals, with **2,886 checks passed and zero failures**. Interframe
p50/p95/p99 were 17.250/18.250/18.500 ms; maximum was 65.040 ms. Sampled app-only
RSS peaked at 172,288 KiB and ended at 90,016 KiB across 3,834 samples with no
sampling errors. The initial 32 KiB sample was process startup, not a steady-state
memory baseline, so it cannot establish retained-memory growth. Process wall time
was 3,877.822 seconds, exit status was zero, and disposable database cleanup
completed. This was the owned launch-time binary snapshot with the fake backend,
not final-code or sustained real-model acceptance. Interframe timing includes
waits/vsync; service RSS and OS input latency were not measured. The requested
60-minute frame-count workload took about 64.6 measured minutes on this run.

**Still required:** saved-document-revision retrieval, T1 source-linked briefs,
optional measured T2 checkpoints, shared generative-background admission before
summary work, workload-wide deadlines/in-flight cancellation, old-generation
cleanup, broader multilingual/held-out recall comparisons, real-service memory
and sustained-use gates. The memory feature is not default-on and GAP-003 remains
P0. The other four product gaps remain deferred.

[MEMORY-007](MEMORY-007-saved-revision-authority.md) supplies the next saved-revision
authority prerequisite without broadening this chat-only retrieval experiment.
