# Memory foundation — DB history windows and bounded layouts

**Status:** implemented locally; retrieval, ingestion jobs and summaries remain
future slices. This extends [MEMORY-003](MEMORY-003-implementation-progress.md).

## Author-visible behavior

- Opening a database-backed conversation loads its newest window, not the full
  archive. **Older**, **Newer** and **Latest** replace the current window.
- A display window has at most 64 messages and 64 KiB of body text. Each displayed
  body is at most 8 KiB. Large stored messages are explicitly labeled as excerpts,
  with character positions and **Earlier part / Next part** navigation.
- **Copy excerpt** copies the original text of that range. An excerpt cannot be
  applied as a whole-reply replacement or turned into a note/canon source. A
  complete in-memory reply can still be copied/applied in full even when its
  layout is only a bounded display preview; the UI labels that distinction.
- Sending from an older window returns to the newest window before building the
  request. Input and the author's current explicit attachment choices are kept.
  The chat engine independently refuses to send from an older window.
- Stored message sequences, not display positions, identify new note/canon
  sources. Opening a chat source loads its exact sequence and codepoint offset,
  including sources outside the current window or within a large original.
- A failed save prevents paging/replacement and subsequent sends from silently
  losing unsaved DB turns. No automatic retry is introduced for an uncertain SQL
  commit; such retries would require an idempotent append protocol. Unsaved DB
  chat remains bound to its original project after a document-project switch;
  New chat and close require a second action to confirm discarding it.
- Note/canon links are withheld for unsaved DB replies. Copy and reviewed edits
  can still preserve/use a complete reply without inventing a persisted source.

These are history-browsing/resource changes, not automatic long-term recall.
The request builder still uses recent eligible history and attached documents.
Archived excerpts are excluded from model history, and omitted older history is
reported even when it is outside the loaded window. Remote requests continue to
exclude local turns; only eligible turns in the recent loaded window are
available, so sparse older remote exchanges can be omitted. No new remote memory
retrieval, data exit, provider fallback or model change is introduced.

## Ownership and invariants

`ChatMessage` now distinguishes a known persisted DB sequence from a window
position and carries explicit display-range metadata. Existing file serialization
does not invent those fields. The full DB loader also preserves sequences for
compatibility consumers; it remains a full loader, not the new UI path.

`chat_store_append_sequenced()` returns the sequence allocated under the existing
conversation lock, **after successful commit**. File-backed calls return no DB
sequence. The UI marks saved turns only from that receipt. Notes/canon use this
identity, never a guessed position or `MAX(seq)` queried after the append.

`chat_store_window()` resolves a one-shot `ProjectStore`, performs scoped keyset
selection, and hydrates owned bounded ranges. Its database connection never
escapes. Metadata selection is capped at 64 rows; body hydration uses fixed
batches of at most eight SQL IDs and a per-batch budget bounded by the remaining
page space, plus at most three lookahead bytes per row for UTF-8 clipping.
Missing/mismatched batch rows, malformed metadata, embedded NULs and oversized
provider labels fail the entire page, without replacing the visible conversation.

Paging swaps an owned, validated window only while idle, after persistence
callbacks. Failed allocations leave the old conversation intact. Pending reviewed
edits keep their request indices stable while the worker runs; paging is refused
then. After callbacks, only saved prefix rows may be evicted. Layouts and queued
positional actions are reset after a window swap or eviction.
Clicks also require a matching fresh presentation; a transition-frame action is
ignored until the replacement geometry/content has been drawn. Empty Older/Newer
requests keep the current window instead of replacing it with a blank view.

Normal retained saved history is capped at 64 rows / 1 MiB body text, protecting
the latest two turns even if larger. A request can temporarily add two rows. The
existing current-request and response byte limits still apply. Unsaved legacy
adoption failures prioritize preservation over this normal retention target;
those records are not discarded to make a memory counter look bounded.
Excess message-slot capacity is reclaimed after eviction, with a data-preserving
retry if shrinking fails. Clear releases message arrays and completed request
contexts after joining the worker. Persistent allocation failure can retain an
oversized allocation and is not treated as successful hard-cap enforcement.

`ChatView` hashes/decodes/wraps at most 8 KiB per message. It retains only the
current window's layouts and reclaims them on replacement/clear. Cache rebuilds
are checked and staged: allocation failure shows an unavailable layout rather
than stale text or an invalid cache pointer. Input text is independent of window
replacement. Retention accounting avoids re-scanning large unchanged bodies on
every frame.

## Attachment restoration

The nearest user among the 64 bounded metadata candidates supplies current
document names (at most eight names of fewer than 256 bytes each). These are not
historical attachment-revision snapshots. Ordinary current-name, same-project
and listed-document checks still apply. Browsing pages does not change the
author's selected attachments.

If the metadata window contains no user and may have older history, attachment
restoration is explicitly unknown: the UI clears selection and asks the author
to choose attachments, rather than scanning the entire archive or treating an
unknown historical selection as consent. Too many/oversized stored names likewise
make restoration explicitly unknown, without blocking body browsing or silently
using a partial attachment list. SQL/read/schema failures still fail the page.

## Validation and measurements

- Real MariaDB fixtures grow through 100, 1,000 and 10,000 messages, with sequence
  gaps. Latest/older/newer/focus windows remain capped, and appends return the
  actual committed sequence. A note linkage test distinguishes sequence 29,998
  from display position 63.
- A varied 24,000-byte Unicode original reconstructs exactly through three
  codepoint-addressed excerpts. Invalid focus positions, missing sequence IDs,
  wrong projects, embedded NULs and oversized provider metadata are covered.
- A new disposable store UI fixture has 400 messages. It verifies navigation,
  attachment restoration, latest-context sending, an old source at character
  offset 9,000, blocked excerpt application, and new-note source reopening.
- Headless view tests replace 100 windows without accumulating layouts. A stress
  shape with 64 saved 8-KiB bodies, a 64-KiB current author message and a 16-MiB
  response retains **17,367,040 body bytes + 28,391,088 layout/cache bytes** in
  the tested font/layout configuration (45,758,128 combined, below 64 MiB).
  A single 16-MiB body uses under 1 MiB of layout cache and its original remains
  intact. This measures these owned buffers, **not whole-process peak RSS**.
- Single and persistent allocation failures are swept through layout rebuilding.
  Existing history selector properties and all five mutation checks still pass.

One warm local diagnostic run (median of three calls per API, no timing gate):

| Messages | Full-load body bytes | Window body bytes / rows | Full load | Latest window |
|---|---:|---:|---:|---:|
| 100 | 990 | 640 / 64 | 0.79 ms | 3.76 ms |
| 1,000 | 10,890 | 704 / 64 | 1.63 ms | 4.71 ms |
| 10,000 | 118,890 | 768 / 64 | 8.77 ms | 4.69 ms |

The initial per-message SQL round trips motivated bounded batch hydration.
Small conversations can still be slower than the legacy full loader. These
fixtures have short messages; these medians are not p95/p99, cold-start, whole-book
or contention measurements and are not SLAs. Reproduce with `make dbtest`.

Validation commands include `make all test dbtest`, `make sanitize`, DB integration
under ASan/UBSan, `make tsan`, `make uitest uitest-db`, `make llmtest uitest-real`,
`python3 tools/test_memory_mutations.py` and Clang analysis of the four changed
adapter/history/layout translation units. All completed successfully in this
implementation pass; the four analyzed translation units emitted no diagnostics.
Evidence logs are `build/window-*.log`. These are local results, not hosted CI.
The subsequent red/green audit, pointer/retention fixes and unsaved lifecycle
checks are recorded in [REDTEAM-015](REDTEAM-015-history-windows.md).
Stopped-reply metadata now survives file/DB persistence, bounded readers and
compatibility import/export (schema migration 008). Older records default to
not-stopped; a stopped flag lost by earlier versions cannot be reconstructed.

## Explicit remaining work

- Loads are bounded in result size and SQL operation count but still synchronous
  on explicit UI navigation. Slow queries/service timeouts can stall that action.
  An owned asynchronous loader, cancellation and measured UI-latency gates remain
  necessary before claiming outage responsiveness or the proposed p95 SLA.
- File-backed conversation state, the observer, importer/exporter and backup
  compatibility paths still use their existing full-history readers. This does
  not establish bounded host memory for every feature/process or unbound legacy
  adoption. No consumer was silently switched to incomplete history.
- A window is not a transactional snapshot against historical edits. Epochs,
  source/version admission, durable jobs/outbox and immutable Qdrant projections
  are prerequisites for derived memory publication, not properties of UI paging.
- Whole-manuscript workloads, cold/warm service memory, frame/input percentiles,
  cancellation/backlog pressure and hours-long sustained use remain GAP-003 gates.
  Retrieval-only planning, T1 briefs and gated T2 comparisons remain unimplemented.
