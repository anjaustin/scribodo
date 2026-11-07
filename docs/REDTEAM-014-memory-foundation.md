# Bounded-history foundation — adversarial review

**Date:** 2026-11-06

**Base commit:** `3e7add8`; reviews the subsequent memory research/design and
bounded-history implementation. This does not certify the unimplemented memory
MVP. The pre-existing sample-document edit is excluded from commits and preserved.

## Findings and remediations

### Mandatory request could disappear

The bounded selector skipped a newest user message marked error or missing its
remote-provider marker. It could return success with only older history or no
history, contrary to its mandatory-current-request contract. A newest assistant
was also silently discarded rather than rejected.

The selector now fails closed unless the newest entry is a non-error,
non-streaming user turn with content and, in remote mode, a nonempty provider
marker. Three focused regressions failed before remediation. Additional tests
cover streaming flags, byte boundaries, private local history, sparse remote
history, orphaned assistants and 1,000 deterministic randomized fixtures.

### Chat load lost metadata and was non-atomic

`chat_load_messages()` cleared the current conversation before allocating the
replacement, ignored attachment/provider allocation failures and discarded
timestamps, truncation and stopped flags. These adjacent pre-existing defects
undermined the metadata assumptions of the new history foundation.

Loading now stages and validates a complete owned replacement, preserves stored
metadata, and changes the open conversation only after all allocations succeed.
On failure, the original transcript remains intact. A loaded conversation cannot
silently lose its remote-provider marker under memory pressure. Loaded entries
are finalized rather than restarted as streaming messages.

### Send preparation left partial turns

`send_common()` failed to check some attachment and remote-marker allocations and
could leave one or two partially initialized messages after returning failure.
The first push failure also produced an invalid unsigned user index.

Every mandatory context/attachment/provider allocation is now checked. Failed
preparation rolls back appended messages, preserves the previous assistant target
and never marks a new turn ready for persistence. First-push failure exits before
using a user index. Capacity growth and joined-name size arithmetic are checked.
Regression sweeps cover single and persistent failures for no attachments, one
attachment, multiple attachments and a loopback-only remote provider. No external
provider is called by these tests.

The combined metadata/load/send fault tests produced **46 assertion failures in
803 checks** before remediation. The remediated fault suite passes, including
ASan/UBSan. This covers allocation failures during preparation, not an OS-level
`pthread_create()` fault campaign or arbitrary concurrent UI API calls.

### SQL high-water lookup was not a bounded seek

The initial `MAX(seq)` query used an aggregate over a join. EXPLAIN on the real
MariaDB fixture selected a `ref` access over the 132-message conversation, rather
than optimizing the aggregate away. Bounded returned text alone did not bound
this lookup's work.

The initial cursor now uses `ORDER BY seq DESC LIMIT 1` on the existing
conversation/sequence index. A real-service plan regression requires `msg_seq`
selection and no filesort. Invalid terminal cursors (past the captured endpoint,
or nonzero offsets after completion) fail empty. This is not a constant-time
database/storage-I/O guarantee for arbitrary LONGTEXT bodies or a snapshot
against historical edits.

### Notices concealed simultaneous limits

Document truncation previously hid the new history-limit notice. A combined
notice now reports both when both limits bind; a worker integration regression
forces both. Clearing a session also clears stale notices.

### Publication hygiene

The research workspace contained roughly 118 MB of duplicate read-only upstream
downloads. Narrow Git ignore entries retain those caches locally without
vendoring them into Scribodo. Authored notes, pinned-source report and a
[provenance/reproduction note](../research_notes/How%20billion%20context%20works/PROVENANCE.md)
are committed instead. No upstream code was installed or executed.

## Validation

The following pass after remediation:

- `make -j8 all test dbtest`: build, headless tests, allocation sweeps and real
  MariaDB/Qdrant integration.
- `make sanitize`: ASan/UBSan headless suites, including the new property suite.
- Database integration rebuilt with ASan/UBSan via the command in
  [MEMORY-003](MEMORY-003-implementation-progress.md).
- `make tsan`: existing concurrency suites exercise the remediated worker path.
- `make uitest uitest-db`: ordinary and database-backed scripted UI acceptance.
- `make llmtest` and `make uitest-real`: the existing pinned local BF16 model
  passes adapter/restart checks (17 expectations) and three real-model UI scripts
  (35 expectations). These are compatibility checks, not a memory-recall benchmark.
- `python3 tools/test_memory_mutations.py`: unmodified control passes; all five
  weakened selectors are rejected by the standalone regression suite.
- `python3 docs/models/memory_mvp_model.py`: the separate finite **design** model
  still passes; it does not validate production database or model behavior.
- Local documentation links, new-file whitespace and `git diff --check`.

Mutation cases remove the mandatory-current guard, admit local text to remote
history, remove the scan bound, remove the byte guard, and retain orphaned
assistant turns. Mutation sources/binaries/logs are generated only under
`build/memory-mutations/`; compilation errors do not count as killed mutations.
Local review logs are `build/memory-redteam-*.log` and are intentionally not
versioned. Hosted validation, when available, is tied to the pushed commit rather
than inferred from these local results.

## Residual scope

Full-history conversation loading and UI caches remain unbounded. No ingestion
worker, SQL outbox, Qdrant memory projection, generated briefs, dispatch authority
snapshot, or real-model recall experiment has been implemented. Cursor reuse
across a different project/conversation is outside the API contract; cursors are
not serialized authorization tokens. Historical mutation/epoch checks remain a
prerequisite for publishing derived memory. These are explicit next slices, not
silently waived acceptance criteria. The supported UI-thread ownership model is
unchanged. This review found no additional unresolved defect within the bounded
foundation's tested scope; it is not a claim that Scribodo is defect-free.

## Final review iteration

After implementation commit `3a4fa35`, repeated mutation checks still rejected
all five weakened selectors. Clang static analysis was also run over `chat.c`,
`chat_history.c` and `store_chat_history.c`. The two new modules were clean.
Four warnings in the adapter depended on null-safe cJSON predicates or successful
prior title-allocation checks that the analyzer could not establish across those
calls. Explicit pointer/value guards and a checked join now make those safety
conditions local; all three analyzed translation units emit no diagnostics.
These were defensive/analyzability refinements, not newly reproduced crashes.

The final code was revalidated with the same build/unit/database, ASan/UBSan,
TSan, ordinary/store UI and pinned real-model adapter/UI matrix above. Local
documentation targets/anchors and staging were checked separately. The authored
research/design/implementation files are committed; downloaded upstream caches
remain ignored and the unrelated sample edit remains the sole excluded working
tree modification. Hosted CI results are reported after push, not pre-claimed.
