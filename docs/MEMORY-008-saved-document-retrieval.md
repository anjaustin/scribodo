# Explicit saved-document memory retrieval

**Status:** experimental, default-off retrieval; no generated briefs. GAP-003
remains P0. Extends the chat pipeline in [MEMORY-006](MEMORY-006-retrieval-pipeline.md)
and saved-revision authority in [MEMORY-007](MEMORY-007-saved-revision-authority.md).

## Eligible sources and admission

The enabled conversation can now retrieve excerpts from **currently explicitly
attached, saved local revisions**, alongside older chat. There is no automatic
project-wide, historical-attachment, observer, notes/canon or cross-conversation
document retrieval. A prior `message_documents` filename is not a revision receipt.
Remote and reviewed-edit requests still bypass new memory before local access.

The request copies attachment names and dirty flags, then the worker resolves
saved SQL identities and verifies actual disk bytes/path identity. An unsaved
AI/restore/source-history revision is never a substitute for the saved receipt.
Legacy/imported documents without that receipt must be successfully saved first.
Dirty buffers remain explicit foreground input only; missing/changed/nonregular/
symlink files are not admitted. Names are never truncated into another eligible
filename; missing/overlong titles cannot authorize an `untitled` or prefix file.

The plan owns a frozen set of at most eight document/revision/hash identities.
Lexical and vector document filtering intersects that set **before** candidate
limits. SQL hydration verifies current saved revision, hash, ownership, consent,
epochs/generation, tokenizer and database origin. Chat-only API wrappers reject
document sources. Documents do not use chat sequence/tail cutoffs.

After exact complete-request token counting, admission recaptures saved metadata
and actual files, compares the frozen set, and rechecks scope/store identity and
the attachment epoch. Attach/detach/rename/save changes fence preparation. The
prose editor also invokes a callback **before** mutation, including restore,
accepted edits, undo and programmatic replacement; polling dirty state after a
blocking history write is not sufficient. This remains an as-of admission point,
not a guarantee to retract already admitted model bytes.

## Durable bounded processing

Migration **011** adds typed chat/document units with mutually exclusive source
shapes and composite source ownership constraints. Scope-local document receipts
record explicit revision intent, byte progress/completion, bounded retries,
DB-clock leases and monotonic fences. They do not advance the chat cursor.

The owned worker caches verified attachments by binding version and scope
generation rather than rehashing files every frame/ingestion step. It performs
at most one chat step, one document step and one generic index delivery per
resource iteration. Document processing publishes at most one <=2,000-byte /
512-measured-token unit, with a <=2,003-byte source window. Tokenization is outside
transactions; publication rechecks scope/source/lease/hash/fingerprint and commits
unit, outbox and receipt together. Unknown counts and stale/expired jobs do not
advance completion. Originals and old derived units are retained.

Saved revision replacement invalidates old document hydration/acknowledgment.
Detached documents are excluded from new recall; retained derived artifacts are
not authorization. A bounded local step already admitted before a binding change
can finish, but the next step must match the selected binding; resource pauses do
not promise arbitrary in-flight I/O interruption.

Generic outbox claim skips/retires at most **64 jobs per call**. Committed stale
retirement reports maintenance progress, so a superseded large document does not
force one second of idle pacing per stale chunk ahead of ready work. Acknowledgment
validates the original locator hash, including termination, before copying it.

## Projection, request budget and provenance

Typed locators use separate **memory_v2** Qdrant namespaces. The older v1 index
cannot silently stand in for source-kind/revision-aware filtering; missing v2
collections use existing SQL generation/reseed recovery. Old namespaces remain
derived cleanup debt. Payloads contain locators/kind/revision identity, never
document text or author-controlled titles, and SQL stays authoritative.

Chat plus document recall share the same **32 total candidates, eight excerpts,
16 KiB rendered reference and 4,096-token optional reference cap**. The complete
final template is still counted before dispatch; current author words/explicit
document content are not shortened to make memory fit. Document reference labels
include immutable IDs, saved revision/hash and byte ranges. Names are neutralized.
Persisted read-only Sources remain as-of prepared context, never canon or model
history, including after detach or later saves.

## Validation and review

Local normal/real-DB tests, ASan/UBSan (including DB integration), TSan, ordinary
and store UI, pinned real-model checks and backup/restore pass. Clang analysis of
the document capture/store, generic store/worker, planner and chat reports no
diagnostics. Pipeline mutations pass two controls and kill thirteen weakened
protocols, including document authorization and detach checks.

The document SQL suite covers 968 checks: typed ownership/shape, partial UTF-8
ingestion/restart, receipt/outbox atomicity, tokenizer changes, lease expiry and
fencing, same-ID/same-length source mutation, current-saved versus unsaved-history
identity, stale source rejection, retries, and 129 stale jobs preceding ready work.
Planner/vector normal and allocation-fault tests cover typed references, poisoned
locators, pre-limit revision filters, detached sets and the shared budget.

Store UI fixture 13 passes **19 checks**: a saved `cobalt` fact appears with a
revision-backed source; detaching or dirtying the document excludes it; saving
new `violet` text admits the new revision and excludes the old fact. This fake
model fixture checks source admission, not reasoning quality. The existing real
oracle-source control answers `amber` with **133 predicted prompt tokens** matching
the server receipt after the reference-header change; it is not a whole-document
retrieval quality benchmark.

Review fixes include pre-mutation dirty fencing, title-alias rejection, bounded
stale-job retirement and malformed acknowledgment hash rejection. Filesystem
capture also has independent deterministic replacement/cancellation tests.

## Limits and next P0 work

Each file verification streams at most 16 MiB; eight attachments can therefore
require up to 128 MiB of disk reads per capture, with final admission recapture.
These are bounded bytes, **not** a measured planner deadline or storage-I/O SLA.
Long filename decoding, very large source boundaries, external changes and native
request-preparation interleavings need broader fault/quality fixtures. SQL LIMIT
does not prove bounded server substring/scan cost. Old-generation/namespace GC
and workload-wide deadlines/preemption remain unfinished.

The earlier hour-long soaks ran the prior chat-only binaries and do not validate
hours of this new document path or real inference. A new short v2 chat-pipeline
recovery run passes; it is compatibility/resource evidence, not a document soak.
Next: T1 source-linked briefs with shared low-priority generative admission, then
optional measured T2 and held-out multilingual/whole-manuscript quality/resource
comparisons. Nothing here makes the experiment default-on or completes the MVP.
