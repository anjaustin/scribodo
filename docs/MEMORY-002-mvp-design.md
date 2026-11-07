# MEMORY-002 — MariaDB and Qdrant memory MVP

**Date:** 2026-11-06

**Status:** LMM design baseline; implementation has begun. See the
[implementation progress](MEMORY-003-implementation-progress.md) for shipped
pieces, test evidence and remaining work.

**Priority:** GAP-003 / P0. The objective is a useful writing session that can
keep growing without resets or manually repeating the book, while active memory
and foreground work remain bounded.

## 1. Method and result

Applied [Lincoln Manifold Method](https://github.com/anjaustin/lmm/blob/55719c0a2308d1a3f29ca656ed7b6287be33bca2/LMM.md)
at `55719c0a2308d1a3f29ca656ed7b6287be33bca2`: RAW -> NODES -> REFLECT ->
SYNTHESIZE, with an additional adversarial TEST/MODEL/REFINE loop before synthesis.
The phase records below preserve scope, observations, decisions and verification
results; they are not private deliberation transcripts.

**Decision:** build an experimental native memory planner, not a general proxy.
MariaDB owns original sources, eligibility, brief provenance and durable jobs.
Qdrant proposes candidates; it cannot establish truth, permission or freshness.
Use independent segment briefs (T1), exact source recall and a bounded checkpoint
level (T2). Enable T2 by default only if its comparison arm earns its cost.

This implements the useful memory lifecycle from the research without inheriting
a generic agent harness, three-tier framework, independent transcript database,
Node sidecar or new summarizer model. Native-first is still a testable hypothesis,
not a measured performance/quality winner.

## 2. MVP scope and author contract

### Included

- Feature-flagged **local** memory for one current, explicitly enabled
  conversation in a database-backed project.
- Eligible, finalized local user messages and successful assistant replies as
  chat sources. Failed/stopped replies are retained in history but excluded from
  generated memory. Failed-turn classification must not erase the user's request.
- Current explicitly attached saved document revisions as retrieval sources.
  Unsaved editor text stays an owned foreground input, not a persisted brief job.
- T1 summaries of completed chat segments; optional T2 summaries of a small,
  ordered group of T1 briefs. Document summarization is not part of this first
  brief pipeline.
- Source-level recall through bounded lexical/semantic candidates and immutable
  original excerpts, restart/resume, index recovery and paged host history.
- A visible memory state and a source list for each reply. Briefs never change
  manuscript text or become canon.

### Explicitly excluded

- Cross-project or automatic cross-conversation memory, undisclosed observer
  findings, automatic notes/canon ingestion and implicit project-wide access.
- New memory retrieval in remote-provider requests. Those keep their existing
  recent-history/explicit-attachment behavior; a provider switch cannot export
  local briefs. Remote memory requires a separate consent/lineage design.
- Automatic memory for file-backed/store-off projects. Those keep their existing
  behavior and show that this feature requires the project store.
- Autonomous model-managed tool loops, a third hierarchy level, raw-source GC,
  automatic canon promotion, and exhaustive whole-book analysis from top-k hits.

Detaching a document removes that document from future automatic retrieval. It
does not rewrite facts already said in the chat. Existing `message_documents`
records identify documents, not the exact historical revision the model saw;
do not present them as historical revision evidence. Historical attachment-view
snapshots require a later, explicit design.

## 3. Architecture

```text
finalized message / saved eligible revision
        |
        v
MariaDB: source commit + durable ingestion intent
        |
        +--> bounded ingestion --> immutable source chunks + index jobs
        |
        +--> background summary jobs --> T1 briefs --> optional T2 checkpoints
                                          |                  |
                                          +-- provenance ----+
                                          +-- index jobs
                                                 |
                                                 v
                                  idempotent Qdrant delivery

current request + source scope
        |
        v
bounded recent page + lexical candidates + Qdrant candidate IDs
        |
        v
MariaDB eligibility/version check + canonical text hydration
        |
        v
deduplicate/select -> exact final token budget -> frozen context plan
        |
        v
existing local inference adapter -> existing reviewed-edit/author-authority flow
```

Use the existing C11 application, owned `StoreManager`/`ProjectStore` routing,
per-thread connections, pinned BF16 LFM3 runtime and pinned local embedder. Do not
cache a borrowed `Db *` across operations/threads or hold transactions while
calling a tokenizer, model or Qdrant.

Introduce an owned joinable `MemoryWorker` and a small shared background admission
gate for observer/memory generation. Only one generative background task is
admitted at a time; interactive chat/edit work gets priority and can request
cancellation. The existing two model slots are not two independent GPUs. Actual
preemption latency must be measured, not assumed from an HTTP cancellation call.

## 4. Authoritative data model

The following are **schema contracts**, not executed DDL. Allocate the next
migration number at implementation; existing migrations and data stay intact.
Keep prepared statements, checked decoders and existing ID/text collation rules.

| Proposed table | Essential fields / invariant |
|---|---|
| `memory_scopes` | scope ID, project/conversation IDs, enabled/local policy, monotonic `history_epoch`, `policy_epoch`, ingestion cursor, T1 coverage frontier/version, index generation |
| `memory_units` | immutable unit ID, scope/project, source kind/ID/version, message seq or document/revision ID, chunk ordinal, UTF-8 byte range, source/text hashes, bounded canonical chunk, tokenizer fingerprint/count, live/tombstone status |
| `memory_briefs` | immutable brief ID/version, scope, T1/T2 level, source span, input digest, history/policy epochs, model/prompt/schema versions, bounded summary, token count, validity state; never canon |
| `memory_brief_inputs` | ordered unit inputs for T1 or child-brief inputs for T2, with captured versions/hashes; one input kind per edge, same scope, fixed maximum fan-in |
| `memory_jobs` | kind, scope, immutable input/version digest, state, attempts, retry-after, lease owner/deadline, monotonic fence token, last error, bounded progress/receipt metadata |

Job kinds include ingestion, T1 generation, T2 generation and index upsert/delete.
Index jobs are the **transactional outbox**, not a second queue that can be lost
between the source commit and enqueue. Completed ingestion receipts remain until
the contiguous cursor safely passes them. Queue compaction cannot erase missing
work. Keep ingestion, summarization coverage and index progress distinct.

### Keys and indexes

- Reuse original message/revision identities, not array positions or filenames.
  Original chat bodies are append-only through the application; any future
  historical mutation/deletion must advance the scope epoch in the same SQL
  transaction. Ordinary new messages do **not** invalidate existing briefs.
- Enforce same-project/same-scope relationships with composite keys/foreign keys
  where needed, not just a caller convention. T2 can reference only T1 in the same
  scope/epoch; T1 can reference only original units. This makes cycles impossible
  without introducing a general recursive graph engine.
- Use unique keys for `(scope, source identity/version, chunk ordinal)` and
  `(scope, level, input digest, summarizer/prompt/schema version)`. Job retries
  cannot duplicate chunks or briefs. A competing publication also needs a
  successful frontier/version compare-and-swap in its transaction.
- Add keyset indexes for `(scope, message_seq, chunk_ordinal)`, source/version,
  eligible brief spans, and `(state, retry_after, id)` job admission. Use seek
  cursors, not OFFSET pagination or timestamps as order.
- A MariaDB FULLTEXT candidate index over bounded chunk/brief text is the first
  lexical experiment. It is **not** guaranteed substring or Unicode/short-name
  lookup. Test stopwords, short identifiers and CJK explicitly; expose unsupported
  or incomplete search rather than claim exact-match recall. Exact source-by-ID
  retrieval always means the original text, not an approximate vector payload.

Chunk materialization is derived from original immutable source versions. Hashes
detect ordinary mismatches; they are not authentication against a user who can
rewrite the database. All historical mutation paths must participate in epoch
invalidation; do not infer freshness from timestamps.

## 5. Qdrant projection and the SQL/outbox boundary

Use a dedicated memory collection namespace compatible with existing test/backup
cleanup: `<database>__<project>__memory_v1__<embedding fingerprint>__<generation>`.
Do not mix embedding models/dimensions or silently reuse observer visibility
filters. Source/brief vectors share the pinned embedder and metric, but have
explicit kinds and scope filters.

Payload is a **locator**, not the text authority: project/scope/conversation,
unit or brief ID, immutable source/brief version, text hash, embedding/schema and
collection generation, and retrieval kind. Do not copy private text into a new
authoritative vector payload. Fetch canonical chunks/briefs from MariaDB.

Derive point IDs from immutable artifact/version plus embedding/index generation.
A delayed old upsert cannot overwrite the current version's point. It may leave
a stale point; SQL validation still rejects it. Cleanup is eventually consistent,
not an authorization boundary. Payload filters reduce candidates but never
replace the SQL check.

### Publication protocol

1. In one SQL transaction, commit source/chunk or ready brief metadata and durable
   index intent. An absent vector must not roll back an author's saved message.
2. Claim a bounded job batch with a lease and incrementing fence under row locks.
3. Outside SQL, compute embeddings and idempotently upsert/delete points, using
   Qdrant's acknowledged-write path for the selected generation.
4. Acknowledge only successful jobs in SQL, conditional on the same current fence,
   target version and generation. A lost acknowledgment retries the same point ID.
   Partial batches do not mark all jobs done.
5. A missing/reset collection allocates a new generation and pages through live
   SQL units/briefs to enqueue rebuild work. Existing marks alone cannot establish
   index completeness. Generation/cursor changes are durable and restartable.

No distributed transaction or exactly-once external-write claim is required.
Deleting a source first makes it ineligible in SQL and invalidates dependent
briefs/epochs, then queues derived cleanup. A late old index delivery cannot make
it eligible again. Backup restore/fork must remap scope/source identities and
rebuild vectors; test the namespace and generation hooks explicitly.

## 6. Ingestion, summaries and coverage

### Ingestion

Finalized source commits enqueue durable work in the same transaction. Chunking
and token counting happen off the UI thread in bounded byte windows. Large
messages are streamed/paged from the stored source, not copied as one unbounded
LONGTEXT result. Publish each bounded page idempotently and mark a message's
receipt complete only after its chunk set is complete. Partial ingestion is not
a complete source. Error/ineligible messages need explicit classification
receipts so progress can distinguish them from failed reads.

Advance the ingestion cursor only through consecutive complete/classified source
receipts. Do not set it to `MAX(completed_seq)`. Likewise, T1 coverage advances
only over the exact next completed, eligible packet, with successful exchanges
kept pair-safe. Qdrant completion is a separate concern, not this watermark.

### T1 and T2

- Select bounded finalized chat units outside the protected recent tail. Capture
  their immutable identities/hashes, ordered coverage, epochs and job fence.
- Summarize locally with a separate reference-data prompt, requiring compact
  continuity/decisions/open-questions fields and source unit references. Do not
  use observer findings, privileged model instructions or unscoped project text.
- Validate complete output, source membership/range, output limits and useful
  shrinkage. These checks do **not** establish semantic truth. Failed validation
  retains raw sources, queues a bounded retry and leaves a visible backlog/error.
- Publish the brief, input edges, frontier compare-and-swap and index intent in
  one SQL transaction only if its source/policy epochs and worker fence still
  match and the claim remains active/unexpired under the database clock. Check
  these conditions under the same locked publication transaction. A stale worker
  cannot publish just because its old lease once existed.
- T2 consumes at most eight consecutive ready T1 briefs. It adds a checkpoint;
  it never deletes the children or originals. Selected prompt representation can
  prefer the checkpoint, while source-level recall remains available.
- Keep only the chosen recent/query-relevant frontier in a request. Cold
  checkpoints remain on disk/searchable; no T3 or forever-growing in-prompt recap
  is needed. T2 drift across repeated checkpoints must be measured.

An exchange exceeding a T1 input cap is split at source-unit boundaries into
explicit continuation packets. Never truncate it or wait for an impossible
pair-sized job. Track chunk-level progress, preserve message/role boundaries,
and advance whole-exchange coverage only after all its packets are classified.
Summary input caps count source/child text; actual template, reference IDs and
output allowance must also fit the effective model window before admission.

All original chunks remain addressable. Expanding a brief is a bounded read of
its source references, with a cursor if necessary; it is not reversing lossy
summary text or mutating the transcript. A very broad task must report partial
coverage or use a separate planned multi-pass workflow.

## 7. Context planner and authorization snapshot

`ContextPlanner` returns an owned immutable `MemoryPlan`: scope/epoch snapshot,
current source versions, selected brief/source IDs, token/byte accounting,
retrieval readiness/coverage status and request provenance. It is not a cached
thread-bound `ProjectStore` or live database rowset.

1. Capture the current local conversation, explicit saved-attachment versions and
   current request/edit passage. Pending foreground turns are bounded owned
   inputs, not dependent on background ingestion catching up; avoid duplicate
   insertion when they acquire persisted identities.
2. Read the newest eligible history through keyset pages with byte limits. Do not
   reload or clone the full conversation. Load a bounded recent brief frontier.
3. Make a single host-controlled recall pass: bounded FULLTEXT candidates plus a
   query embedding/Qdrant search over eligible raw units and briefs. No autonomous
   tool loop or implicit second recall round in this MVP.
4. In short SQL reads, validate every returned identity, project/scope, live
   status, version/hash and brief epochs. Reject stale/wrong-scope candidates and
   hydrate text only from canonical bounded SQL rows. Never use Qdrant text as a
   fallback when SQL cannot validate it. Do not walk the complete summary graph.
5. Merge/rank candidates and deduplicate original excerpts. For quotations or
   disputed details prefer originals over summary wording. Summary context may
   accompany exact evidence, but must not masquerade as a second independent fact.
6. Build and count the final model/template request. Current author instructions
   and the reviewed-edit target are protected. Drop optional memory/older tail
   units to fit; if mandatory content/output cannot fit, explicitly refuse or ask
   to narrow rather than silently change the requested passage.
7. Recheck the scope generation at dispatch admission and record the frozen plan.
   A changed scope before admission invalidates the plan. Admission is the
   authorization/as-of point; cancellation after dispatch is best effort and
   cannot undo a request already being processed. Later source edits do not
   retroactively make this immutable historical plan current evidence.

```text
instructions + current request + protected edit target + recent turns
  + selected briefs + exact excerpts + template/tool framing
  + configured reply reserve + safety margin <= effective fitted context
```

Use the real tokenizer/template and actual `memfit`-selected context. There is no
200K generic fallback. Existing reply settings still apply. Cache counts only
under the matching tokenizer/template fingerprint.

## 8. Bounded resource envelope

These are **initial experimental caps**, not proven production SLAs. Their sum
does not imply that all maxima fit the smallest context simultaneously; the final
token inequality always wins.

| Item | Initial cap / behavior |
|---|---|
| Source unit | <=8 KiB UTF-8 and <=512 source tokens; explicit ranges and boundary-safe splitting |
| SQL/history materialization page | <=64 rows and <=64 KiB of text, whichever binds first |
| Candidate recall | <=32 IDs total across lexical/vector channels; one recall pass |
| Selected memory | <=4 briefs / <=2K brief tokens; <=8 original excerpts / <=4K excerpt tokens, then final fit |
| T1 summary task | <=2K source tokens, <=256 summary tokens |
| T2 checkpoint | <=8 T1 inputs / <=2K input summary tokens, <=512 output tokens |
| Generative background concurrency | one shared observer/memory permit; foreground admission takes priority |
| Job retry | <=3 attempts before visible failure/backoff/manual retry; never advance coverage on failure |
| Planner soft deadline | 500 ms initially; return already validated partial context with explicit status, not unbounded search |
| Summary timeout | 30 seconds per attempt initially; cancel on foreground demand and measure actual preemption |
| History/UI cache | explicit combined 64 MiB cap; bounded viewport pages, including oversized-message display |

Transport/query timeouts and cancellation must support the measured envelope;
checking a deadline only after an unbounded query is insufficient. SQL LIMIT
bounds output, not scan cost: inspect plans and high-frequency lexical queries.
Qdrant top-k/ANN bounds result size, not guaranteed constant query time or recall.
Track MariaDB, Qdrant, embedder and generative-runtime memory separately from the
64 MiB application cache; 16 GB is a whole-machine limit.

No normal raw-source deletion or summary-driven source GC is introduced. Track
disk growth, index size and backup costs and report capacity failures honestly.

## 9. Failure and UI behavior

| Failure/state | Behavior |
|---|---|
| Memory disabled or still warming | Existing chat path/recent context remains usable; explicit status rather than fabricated recall |
| Qdrant unavailable or rebuilding | Validated SQL recent briefs/lexical/source references may be used; mark semantic recall partial; retry index work later |
| MariaDB unavailable for an imported project | Existing structured-store pause contract applies; never trust cached Qdrant material instead; plain document editing remains available |
| Summary invalid, stale, canceled or timed out | No frontier advance or source deletion; retry/backoff within limits; show incomplete memory coverage |
| Vector stale, wrong version or wrong project | Drop it before text enters a model request; incomplete results are not proof that no prior information exists |
| Remote provider selected | New memory planner is bypassed; do not export local briefs, retrieval candidates or summary reference material |
| Source changed/retracted/deleted | SQL invalidation/epoch first; background cleanup/rebuild second; requests carry as-of provenance |
| Scope narrowed after an admitted request | Cancel best effort; apply new scope to new admissions, not a false promise to retract already processed bytes |

The UI distinguishes **ready**, **warming**, **partial**, **unavailable**, and
**disabled**, with source-backed details. "Ready" means the selected plan and
available processing state, not exhaustive recall or semantic truth. A Sources
action opens the immutable passage/version used; no extra canon or notes action
is executed by the memory system.

## 10. Implementation slices and go/no-go gates

1. **Measure and page first:** reproducible fixtures, counters and current-baseline
   latency/memory. Add byte-limited keyset store APIs, windowed `ChatSession` and
   viewport caches so foreground work does not first copy/lay out the archive.
2. **Persist/index safely:** schema migration, bounded ingestion and durable index
   jobs with lease fencing, immutable point IDs, SQL hydration and rebuild hooks.
   No generated briefs yet; ship an experimental retrieval-only comparison arm.
3. **Add T1:** one local low-priority worker and atomic source-linked brief
   publication. Compare retrieval-only with T1 plus retrieval under identical
   models, source budgets, output limits and datasets.
4. **Test T2 explicitly:** implement bounded checkpoint creation/readback behind a
   separate flag. Force checkpoint generation in tests; do not mistake unexercised
   tier code for validated hierarchy. Enable it only if quality/cost warrants it.
5. **Complete the writing loop:** sources/status, disable/rebuild controls,
   cancellation/restart tests, real-model tasks and sustained-use runs. Default-on
   requires the gates below, not just successful source compilation.

Suggested native seams: `app/memory_store` (checked SQL repositories),
`app/memory_job` (owned worker/lease lifecycle), `llm/context_planner`, and a
memory-specific Qdrant locator/rehydration API. Existing `semantic_search()`
payload text must not be reused as an authoritative creative-memory result.
Names are proposals; keep `main.c` a composition root and use existing ownership
patterns. No process-global model/store/queue state is introduced.

### Acceptance matrix

- Golden source/authority cases: quoted rare facts, Unicode names/numbers,
  corrections, chronology, speculation versus canon, and unanswerable questions.
  Test source evidence and abstention separately from plausible prose.
- Five matched arms: current window, retrieval-only, T1+retrieval,
  T1/T2+retrieval, and a single rolling-summary comparator. Add an oracle-source
  control to separate source selection from model answering ability.
- Hold model, tokenizer, effective window, sampling, query/source budgets and
  output allowance constant. Count background summary/embedding/retry work,
  not just final prompt size. Never silently introduce a larger/remote model.
- Stress 100/1K/10K messages first, then 100K if earlier gates pass; manuscript
  fixtures at 100K/300K words, then 1M as stress. Include revisions and records,
  different languages, cold/warm index, restart and an hours-long soak.
- At fixed active context, measure per-turn bytes copied, rows hydrated, cache
  bytes, frame/input latency, planner time, TTFT, backlog, disk and index growth.
  Candidate UI gates remain p95 <=50 ms / p99 <=100 ms; candidate foreground
  inference-regression gate <=10% versus matched baseline. These need agreement
  and real measurements before being called SLAs.
- Fault/interleaving tests: crash between SQL commit and vector write/ack, stale
  vector resurrection, lease reclaim/late worker, duplicate job, partial chunking,
  out-of-order receipts, stale epochs, missing source, Qdrant reset, store outage,
  wrong scope/project, remote selection and cancellation.
- Privacy/authority violations are zero-tolerance in the finite test corpus.
  Define paired per-task recall/fidelity thresholds on a held-out development
  corpus before tuning; larger quality programs remain GAP-001.
- Unit, real MariaDB/Qdrant integration, ASan/UBSan, TSan, UI, offline real-model,
  recovery and mutation suites must cover the implemented slice. The abstract
  design check below is not a substitute for these tests.

**Go/no-go:** use the simplest arm that provides useful continuity and exact-source
support while meeting the agreed resource/privacy gates. If T1 adds no value,
do not force it into the product. If T2 adds drift or latency, keep it off. If the
pinned model cannot meet summary fidelity gates, stop and return the decision to
the author rather than silently alter the model or data-exit contract.

## 11. Verification and remaining decisions

The executable [finite design model](models/memory_mvp_model.py) tests weaker
protocol counterexamples and the refined gates. Run:

```sh
python3 docs/models/memory_mvp_model.py
```

This pass checked 1,941 reachable states through depth 8, including 1,536
captured-job cases, 18 contiguous-frontier transitions and 36 synthetic budget
cases. It found witnesses for stale/wrong-project vectors, SQL outage, scope
revocation, remote reuse, expired leases, changed inputs and maximum-completed
watermark holes. These checks establish behavior **of this small model only**;
they do not prove real SQL/Qdrant concurrency, crash durability, tokenizer fit,
LLM fidelity, latency or recall.

Remaining decisions: agree the initial numeric gates and held-out quality corpus;
confirm whether the first developer slice is instrumentation/paging or a separate
retrieval-only harness; and approve runtime implementation after reviewing this
proposal. No data migration, dependency install, service configuration change,
model call, commit or push is part of this design pass.

## 12. Traceability

- [Synthesis record](../journal/memory-mvp-mariadb-qdrant_synth.md)
- [RAW scope brief](../journal/memory-mvp-mariadb-qdrant_raw.md)
- [Nodes](../journal/memory-mvp-mariadb-qdrant_nodes.md)
- [Decision rationale](../journal/memory-mvp-mariadb-qdrant_reflect.md)
- [Adversarial verification and refinements](../journal/memory-mvp-mariadb-qdrant_turbo.md)
- [Preliminary memory investigation](MEMORY-001-long-lived-context.md)
- [Source-level upstream report](../reports/How%20billion%20context%20works.md)
- Local integration evidence: `src/app/store_chats.c`, `src/app/db.c`,
  `src/llm/chat.c`, `src/ui/chat_view.c`, `src/llm/semantic.c` and
  `docs/STORE-001-design.md`. Where older design prose and current code differ,
  the current code is the baseline, not a claim that new memory APIs already exist.
