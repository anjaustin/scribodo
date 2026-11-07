# Memory P0: bounded resources and source-linked T1 experiment

This slice remains experimental and default-off. Enable local retrieval with
`SCRIBODO_MEMORY_EXPERIMENT=1`; T1 additionally requires
`SCRIBODO_MEMORY_BRIEFS=1`. Neither flag changes consent or makes generated
material canonical. GAP-003 remains P0; other product gaps remain deferred.

## Resource controls

- The application owns one 16-entry saved-document verification cache shared by
  chat and the memory worker. Initial verification hashes actual saved files;
  metadata alone never establishes readiness. Each background step reads at most
  128 KiB, in 16 KiB pieces. Foreground capture/admission checks SQL identities
  and file stamps without reading manuscript bodies. Three failed verifications
  stop automatic body reads until eviction, a new source key, or explicit Rebuild.
- Local preparation has a shared experimental 500 ms budget covering base
  construction/tokenization and optional retrieval. Transport deadlines do not
  reset on incoming bytes. SQL connection admission is asynchronous and bounded;
  an owned guard can shut down a duplicated read-connection socket. Borrowed-Db
  routing never refreshes or closes the caller's connection. The guard is joined
  before connection disposal. Timeout can omit optional memory; cancellation
  must not dispatch. This is not a measured latency SLA or OS preemption promise.
- Experimental explicit-document snapshots have a 64 KiB aggregate cap, with
  visible cutoff framing. Current author instructions are not silently clipped.
- A shared foreground-priority gate cancels observer/T1 background generation,
  blocks new background admission, and fences publication. T1 uses background
  slot 1 without prompt caching. Logical cancellation does not establish that
  llama-server has physically stopped computing: real TTFT/preemption remains
  a separate validation gate.

## Derived-data retirement

Migration 012 adds exact namespace jobs, project retirement floors, and durable
dirty-scope intent. One maintenance step deletes at most 64 total derived rows
across brief inputs, briefs, brief state, outbox, chat/document receipts and units.
Project-wide scheduling includes disabled scopes; cleanup does not enable them.
Original messages, saved revisions, documents and historical Sources snapshots
are never cleanup targets.

Namespace creation records exact SQL intent before HTTP. Retirement commits a
monotonic generation floor before HTTP deletion; future writers cannot reuse
retired generations. Collection creation checks authority before and after PUT.
Completed/failed tombstones are re-audited after a five-minute cooldown, with
three attempts per batch, so a crashed late creator does not erase cleanup intent.
Only known SQL-authorized namespaces are targeted; no collection discovery or
payload-driven deletion is allowed. No SQL locks cross HTTP. Bounded work and
durable retries are not an erasure-time SLA; an unavailable backend retains work.

## T1 protocol

Migration 013 stores generation-keyed frontier/lease state, immutable briefs and
their original input identities. Each brief uses at most four original chat
units, 2,048 source tokens and 256 generated tokens. Documents and prior summaries
are not brief inputs. Publication revalidates consent, sources, hashes, leases,
protocol and admission, then atomically advances the frontier. Failure/preemption
does not advance it; canceled attempts are refundable.

Production rendering labels generated T1 text as noncanonical and supplies exact
original IDs/hashes/ranges, not extra full input bodies. Original excerpts have
priority: whole briefs are dropped first during both optional-reference and
complete-request budget fitting. Final admission revalidates the exact stored
brief and every original input. Remote and reviewed-edit requests bypass recall.

## Evidence and release gates

Normal build/unit/database matrices and the fake-model store UI fixture pass.
Dedicated cache, transport/SQL deadline, gate, brief-store/model and GC regression
tests exist; these establish mechanics, not summary factuality or model quality.

`tools/memory_quality_eval.sh` freezes per-run binaries, datasets, prompts and
scoring. V2 compares recent-window, retrieval-only, T1+retrieval and oracle-source
arms using native production reference rendering and matched budgets. Actual raw
excerpts and pointer-only provenance coverage are reported separately. Setup
failures and zero-answer runs are retained, not interpreted as fidelity evidence.

### First completed real-model DEV run (negative result)

The 12,229-word DEV run completed 12 tasks in each of four arms (48 answers),
using real local model, tokenizer, SQL and embeddings, in 262.811 seconds.
Matched-runtime, source-selection execution and cross-conversation canary gates
passed. Quality did not:

| Arm | Correct answers/abstentions | Raw-source selection macro recall |
|---|---:|---:|
| Recent window | 0/12 | 0.0 |
| Retrieval only | 1/12 | 1.0 |
| T1 + retrieval | 1/12 | 1.0 |
| Oracle originals | 0/12 | 1.0 |

T1 published **zero** briefs: its first attempt consumed 725 prompt tokens and
the full 256-token completion allowance but failed validation. Accordingly the
T1 arm establishes no added summary value. Citation/schema gates also failed;
the poor oracle result is an important generation/control failure, not a reason
to weaken the frozen scoring. No private canary was emitted; that bounded test
does not prove universal privacy. No prompts, fixtures or thresholds were tuned
after these answers.

Retained evidence:
`build/memory-quality/runs/scribodo_test_quality_74894_83bf155543066ab6/`.
Manifest SHA-256:
`b6323fc68d899791b519dba74bdf74c185bf630483dcc5c41dc2b575ddddf970`.
Executed native artifact SHA-256:
`d51e39793cf1648b5bbbed90f46b2acfb00b3178f38cea0530028007db854001`.
Earlier zero-answer setup failures remain separate retained reports.

### Frozen held-out run (negative result)

The unchanged held-out protocol completed 20 tasks in each arm (80 answers) on
115,001 actual words, in 506.486 seconds. Retrieval-only and T1+retrieval each
scored **1/20 (5%)**, recent-window and oracle each **0/20**. Raw-source selection
macro recall was 1.0 for both retrieval arms and oracle, versus 0.0 for recent
window. Citation/schema fidelity and the 90% experimental correctness gate failed.
No private canary was emitted. Runtime/budget matching and held-out integrity
checks passed; a valid experiment can still have a failing outcome.

No T1 briefs published: one attempt charged 720 prompt + 256 completion tokens
and failed validation. Thus the two retrieval arms had identical available
summary coverage; this is **not evidence that a working T1 system adds no value**.
All 1,082 archive units were indexed, with zero sequence/index backlog. Embedding
indexing took 74.558 seconds across 1,083 calls including the private canary;
embedding token usage and service RSS were not measured. The harness is not
native ChatSession/UI timing or sustained-use evidence.

Retained evidence:
`build/memory-quality/runs/scribodo_test_quality_78322_375873d54da63fce/`.
Manifest SHA-256:
`b7ea4613b6e691800dabb2b0b8832e33914a72baf9995a00210b00210b5913aa`.
Executed native artifact SHA-256:
`6574df3f91834fc17d0ffa55bb15785a06fe87aef555e657f6fb13a778996ee7`.
The result is FAIL. Keep retrieval/T1 default-off and investigate the poor
oracle/schema performance before preregistering any revised generation protocol;
do not tune this held-out fixture to green.

Still required: successful quality evidence under a separately registered protocol,
300,000-word stress, sustained real-model use, measured native foreground
regression/preemption, and final hosted validation. Prior fake-backend hour soaks
are not substitutes. T2 is contingent on demonstrated T1 value; no complete MVP
or quality-gate success is claimed here.

## Post-implementation red-team remediation (2026-11-07)

Three review passes covered worker lifecycle/maintenance, borrowed SQL routing,
namespace floor/lease authority, cache invalidation, foreground admission,
brief source revalidation, and HTTP deadline/cancellation paths. Four findings
were remediated:

| Finding | Remediation and regression |
|---|---|
| Enabled scopes skipped cleanup on cold-tokenizer or ingestion-failure exits | Authorized worker exits now share the independently paced maintenance step. Tests inject retired rows on both exits and require cleanup without ingestion/index progress. |
| Retirement fetched/locked all conversation and scope IDs in a project | Use the project lock already required by scope creation/enable/rebuild, then recheck the exact job and active writers. A real SQL regression holds an unrelated author conversation lock while retirement succeeds; no unbounded ID list is hydrated. |
| Known namespace jobs became unclaimable after the final scope disappeared | Exact project/database-authorized claims survive scope deletion. Original job IDs/generation/fingerprint remain mandatory; retirement still adopts a floor before HTTP. Regressions delete the last conversation, reject a wrong database, isolate another project, retire the job, and verify a later scope starts above the floor. Retirement transport no longer requires meaningless live-scope epochs. |
| Deadline checking could overwrite `errno` after `poll()` | Capture the poll error immediately. A deterministic successful-clock/errno-clobber fixture verifies interrupted polls retry and genuine errors retain their identity; the reverted-behavior mutant is killed. |

Validation after remediation:

- `make -j8 all test dbtest`: pass; real SQL GC suite **334 checks**.
- `make -j8 sanitize`, sanitized `dbtest`, and `make tsan`: pass.
- `make uitest-db backuptest`: pass, including T1 mechanical Sources and
  backup/restore acceptance.
- Both memory mutation suites pass; compiled errno-clobber mutant is killed.
- Clang static analysis of GC/job/store/cache, chat/planner, gate/budget/brief,
  and HTTP modules reports no diagnostics after the fix.
- `git diff --check`: pass. Logs are retained as
  `build/memory-redteam-{pass3,final-*}.log`.

No further correctness finding remained in these focused passes. This is not a
proof of bug freedom, a quality-gate pass, a real-model sustained-use result, or
a foreground latency SLA. The negative frozen DEV/held-out results above remain
unchanged; retrieval and T1 remain default-off. The author's pre-existing sample
edit is excluded from the remediation commits.
