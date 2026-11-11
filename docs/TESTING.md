# Testing

| Command | What it runs | Needs a display |
|---|---|---|
| `make test` | Unit and headless suites: core modules, real-filesystem file I/O, and the editor driven through raylib stand-ins (including a 200,000-operation fuzz run) | No |
| `make sanitize` | The same suites under AddressSanitizer and UndefinedBehaviorSanitizer | No |
| `make analyze` | The Clang static analyzer over first-party C/Objective-C translation units; any finding fails the target | No |
| `make analyzetest` | Analyzer-gate regressions: complete C/Objective-C inventory, clean input, warnings, compile errors, missing tool, nonzero exit, diagnostics and stale logs | No |
| `make uitest` | Scripted UI tests: the **real app**, real input handling, real rendering, with a fake model | Yes |
| `make tsan` | ThreadSanitizer over the chat, observer, supervisor and storage code | No |
| `make dbtest` | MariaDB/Qdrant integration in throwaway databases (`tools/services.sh up` first) | No |
| `make llmtest` | The model adapter end to end against the real `llama-server`: verification, stale-socket start-up, auth, streaming, injection neutralization, crash and restart | No |
| `make uitest-real` | `tests/ui/real-*.scribo`: the real model through the real UI | Yes |
| `make dependency-report` | Verifies committed submodule gitlinks against clean checkouts, container digests and OSV advisories; lists available upstream updates without failing on them | No |

### Editor, embedding and remote-config failure regressions

- `test_editor_faults` injects one-shot and sustained allocation failures into
  the real headless editor. A refused range replacement or paste must retain the
  text, selection, content id, dirty state, source anchor and redo branch.
  Exhausting the gap before typing checks that growth failure preserves history
  and reports a notice. Capacity is reserved before stb can delete a selection.
- `test_semantic_faults` drives the real embedding transport against a private
  Unix-socket stand-in. It fails every allocation on successful document/query
  paths, including JSON construction, serialization, response parsing and result
  validation. Failures must report an error without publishing partial vectors
  or sending an incomplete input list. Malformed numeric values, dimensions,
  duplicate/missing indices and reordered batches have independent checks.
- `test_remote: remote_file_reads` bounds FIFO regressions in child processes and
  checks FIFO symlinks, non-regular files, complete configuration/key reads,
  embedded NULs, oversized files and key-length/CRLF boundaries. Only genuinely
  absent configuration is silently treated as unconfigured.
- `remote_json_boundaries`, `embedding_json_boundaries` and
  `complete_json_spans` check complete-input parsing, including trailing garbage,
  a second JSON value, NUL-decoded field/path aliases, literal escaped backslashes
  and exact non-terminated byte spans. These adapters use `json_parse_complete`;
  legacy crash-torn logs retain their separate parsing behavior.
- `remote_service_secret_aliases` uses fake local credentials to check a custom
  `SCRIBODO_SECRETS` directory, symlink aliases and hard links outside that
  directory. The opened key descriptor is compared against known credential
  identities; secret contents are never read for this check.
- Both allocation binaries run in `make test` and `make sanitize`; the threaded
  embedding stand-in also runs in `make tsan`. Fault injection is not compiled
  into the application. Dated baseline and mutation evidence is recorded in
  [`REDTEAM-013-failure-paths.md`](REDTEAM-013-failure-paths.md).

Run native/recovery acceptance serially, for example
`make uitest uitest-db backuptest remotetest cstest offlinetest`. Do not run
another Scribodo process during `backuptest`: restore deliberately refuses to
modify a database while any Scribodo process is running.

### Recall regressions (MEMORY-015 S1)

- Product-memory regressions: `test_memory_chat` in `make dbtest` verifies that
  context-trimmed history remains eligible for recall. `test_persistence_faults`
  in `make test` sweeps one-shot and persistent allocation failures through chat
  and edit construction: any published request must equal the complete control,
  preserving the author's text, selected passage and attached context.
- `test_memory_vectors_faults` in `make test` sweeps one-shot and persistent
  allocation failures through every vector-store operation against a loopback
  stub. Its sweeps assume one fixed allocation sequence per call, so the same
  binary also checks that the HTTP decoder allocates identically whether a
  Content-Length body arrives whole, in KiB pieces or byte by byte
  (`decoder allocations independent of delivery`). A delivery-dependent count
  made one hosted run skip an injected position; see
  [AUDIT-002](AUDIT-002-repository-audit.md). One run opens about 11,700
  loopback connections in under a second, which is most of the ephemeral port
  range; keep that in mind before running it in a tight loop.
- `python3 tools/test_product_memory_mutations.py` needs local test DB services,
  not models. It runs two controls and weakens four guards independently.
  Only assertion failures kill a mutant; crashes, timeouts and build failures
  are errors. Generated sources and evidence stay under `build/`.
- `test_memory_chat` also checks that 32 already-retained recent units cannot
  starve an older original before SQL/vector top-k. Both channels must return
  the same older original under the shared sequence cutoff.
  `python3 tools/test_memory_cutoff_mutations.py` independently bypasses each
  discovery cutoff; it requires SQL/Qdrant but no model servers.
- `python3 tools/test_memory_redteam_mutations.py` checks the additional history,
  sampling, vector-sequence and failed-prefix guards without services/models.
  All three memory mutation tools accept `--output DIR` for a fresh evidence
  directory; existing runs are never overwritten.
- `tests/test_answer_probes.py` (in `make test`) verifies diagnostic source
  bindings, frozen questions, one-factor comparisons, private temporary cleanup,
  process cleanup and invalid CLI bounds without inference. A corrected capture
  must contain `transcript.json`; older captures are refused rather than silently
  borrowing a later run's questions. See
  [REDTEAM-018](REDTEAM-018-memory-contracts.md) for the measured failure controls.
- Seven further Python tests run in `make test` (and therefore under `make
  sanitize`) without models, services or a display. They pin the answer
  diagnostics' instrumentation and control invariants: `test_answer_live_capture`
  (request builders, persona, provider call and search/fusion code unchanged;
  ambiguous anchors refused), `test_answer_live_audit`, `test_answer_live_ablation`,
  `test_answer_live_parity`, `test_answer_manifold_probe`,
  `test_answer_plumbing_arithmetic` and `test_answer_plumbing_contract`. The
  tools they cover are diagnostic only and never change production prompts,
  selection or models; see `journal/answer-bedrock_live.md` and
  `journal/answer-manifold_synth.md`.
- `test_answer_income_isolation` (in `make test`) pins the packet surgery of
  `tools/answer_income_isolation.py`, manifold cycle step 1: exactly one admitted
  source block is replaced by the designated original with its real locator,
  history and numbering stay byte-identical, and mismatched documents or a
  missing quoted block are refused. No models.
- `test_answer_citation_binding` (in `make test`) covers
  `tools/answer_citation_binding.py`, the deterministic quote-to-source check
  used for the `quotation_spans` and `citation_binding` review fields: curly and
  straight quotes pair independently, typographic variants and ellipses are
  normalized, and each quoted piece is classified as bound to the cited source,
  found in another source, found only in the attachment preview, or in no
  supplied text. `tools/answer_boundary_bind.py` binds the fresh boundary
  questions in `tests/fixtures/answer_boundary/questions.json` to exactly one
  manuscript unit each; `tests/ui/store/21_boundary_questions.scribo` asks them
  through the real app with source-delivery checks only. A second, held-out
  fixture over *A Princess of Mars*,
  `tests/fixtures/answer_boundary/princess-of-mars.json`, has its own script
  `22_mars_boundary_questions.scribo`; both scripts are generated from their
  fixtures by `tools/answer_boundary_script.py FIXTURE SCRIPT` (`--check`
  compares without writing). The bind, review and gate-replay tools read the
  fixture named by `SCRIBODO_BOUNDARY_FIXTURE` (default the first), and
  `tools/answer_product_run.py` attaches the parts directory named by
  `SCRIBODO_MANUSCRIPT` (default the first novel's); see
  `journal/answer-cycle4_mars.md` for the held-out rule.
  `tools/answer_boundary_review.py` joins such a run's exported messages, units
  and live capture into a review sheet whose mechanical columns (delivery,
  truncation, lengths, quotation binding) are filled and whose judgment fields
  are left for a human; see `journal/answer-cycle2_boundary.md`.
  `test_answer_boundary_fixture` (in `make test`) checks the fixture covers the
  six required kinds, that no question appears in the seven-question script or
  in the other fixture, that each generated UI script matches its fixture, and
  that the binder refuses ambiguous or missing passages. `tools/answer_product_run.py` is the tracked
  runner that drives a question script through a real-app binary and preserves
  messages, units and the work copy before dropping the test database.
  `tools/answer_clean_review.py` reviews that run's visible replies with the
  pinned model in a fresh window (one unit, question, reply; strict JSON schema,
  temperature zero), with a same-window control and a per-quotation span arm
  scored against a byte oracle; see `journal/answer-cycle2_clean_review.md`.
  `test_answer_clean_review` (in `make test`) pins the arm construction, the
  exported-unit check, the oracle and the scoring without a model.
  `tools/answer_unit_extraction.py` is the two-stage arm: per delivered unit, a
  fresh-window extraction call returns the stating sentence or none; software
  verifies each span against the unit before it is used; a composition call
  sees only verified spans. `test_answer_unit_extraction` (in `make test`) pins
  the plan, the span verification (including the sentence-boundary prefix cut),
  the factual/creative composer split, the embedder ranking math and the
  predeclared reply checks without a model; see
  `journal/answer-cycle2_unit_extraction.md` and `journal/answer-cycle2_ranked_span.md`
  (`--rank`: verified spans scored by the production embedder, top 1 and 2 composed).
  `tools/answer_ranker_instruction.py` re-ranks the frozen span pools and all
  exported units under fixed query instructions on the production embedder, no
  generative model; `test_answer_ranker_instruction` (in `make test`) pins the
  pool reconstruction, rank metrics and the production instruction text; see
  `journal/answer-cycle2_ranker_instruction.md`.
  `tools/answer_live_capture.py --query-instruction none` builds the same
  live-capture app with an empty embedding query prefix for an old-versus-new
  run on the real retrieval path; `tools/answer_live_delivery_compare.py`
  compares two such runs' source delivery per designated passage (semantic and
  lexical candidate rank, admission, presence in the final request), matching
  units across runs by content hash. `test_answer_live_delivery_compare` (in
  `make test`) pins the matching and the sheet; see
  `journal/answer-cycle2_live_instruction.md`.
  The clean-review, extraction and ranker tools take `--dry-run`, which prints
  the arm and expected counts from the data so a predeclaration copies them
  rather than counting by hand (REDTEAM-020).
  `tools/answer_rerank_arm.py` (run from `build/venv-causal`) ranks the
  designated units and stating spans with four rankers: the production
  embedder, content-word lexical overlap, Qwen3-Reranker-0.6B over all units,
  and the reranker over the embedder's top 32. `test_answer_rerank_arm` (in
  `make test`) pins the overlap scoring, the model-card prompt, the pipeline
  shape and the reranker pin without torch; see `journal/answer-cycle2_rerank.md`.
  `tools/answer_rerank_runtime.py` (also from the venv) scores a fixed pair
  set through llama-server and the PyTorch reference for parity and speed
  (`build/rr1`). `test_answer_pipeline` (in `make test`) covers the verified
  pipeline's pure parts: normalization, byte verification with the sentence
  cut, the extraction request and reply, the reranker prompt, request and
  probability parsing, the fixture scorer, the verified block and receipt, and
  the pipeline request builder beside the unchanged production builder, the
  question splitter and the slot matching (cycle 3), and the gate's number
  anchors as whole words and compounds; see
  `journal/answer-cycle2_pipeline_live.md` and `docs/MODEL-002-qwen3-reranker.md`.
  `tools/answer_pipeline_review.py` joins a pipeline live run's messages, units
  and trace lines into a review sheet (chosen unit, designated units' candidate
  ranks and scores, verified span, timings, composer window, reply);
  `test_answer_pipeline_review` (in `make test`) pins the trace and receipt
  parsing without a model.
  `tools/answer_question_parts.py` (no venv; llama-server with the pinned
  reranker GGUF) takes the candidate pools a pipeline live run scored, splits
  each question into its parts in software and scores every candidate
  against the whole question and each part, with the whole-question scores
  checked against the run's logged scores; `test_answer_question_parts` (in
  `make test`) pins the splitter, the clip, the ranks, the slot rule and the
  trace pairing without a model.
  `tools/answer_window_rerank.py` scores sentence windows of each query's top
  units with the same reranker (coarse-to-fine); `test_answer_window_rerank`
  (in `make test`) pins the sentence cut and the window rule.
  `tools/answer_part_claims.py` (llama-server with the pinned chat model)
  replays the production claim request and a per-part variant on the
  pre-gate replies recorded by the pipeline live captures;
  `test_answer_part_claims` (in `make test`) pins the single arm's strings to
  the C claim call and the software analysis (numbers, names) without a
  model. `tools/answer_admission_arm.py` (embedder, then reranker, both
  pinned) scores per-part pools admitted by the embedder with or without
  lexical overlap, ordered by the reranker, the embedder or rank fusion,
  with the two slots assigned as a matching, against today's selection;
  `test_answer_admission_arm` (in `make test`) pins the parts, pools,
  matching and fusion without a model. See `journal/answer-cycle3_relation.md`.
  `tools/answer_coref_arm.py` (fastcoref FCoref or LingMess with spaCy in
  `build/venv-coref`, then the admission arm on the derived units) rewrites
  third-person pronouns in the document units of a live run to their
  clusters' names at ingestion granularity and measures today's selection on
  the resolved text against the original; `test_answer_coref_arm` (in
  `make test`) pins the chunking, the name vocabulary, the representative
  choice, the nesting, the possessive forms, the two densities and the edit
  application without a model. See `journal/answer-cycle3_coref.md`.
  `test_memory_index_text` (in `make test`) pins the index layer's table
  (`src/app/memory_index_text.h`): off without `SCRIBODO_INDEX_TEXT_FILE`,
  loaded from a JSON object of {sha256 of the original unit text: resolved
  text}, resolution by hash with the text itself as the fallback, and refusal
  of malformed files (not an object, bad key, bad or oversized text,
  duplicates, missing file) without touching a loaded table; `test_db_core`
  (in `make dbtest`) carries migration 015. The live measurement is the
  second half of `journal/answer-cycle3_coref.md`.
  `tools/answer_gate_replay.py` with `build/answer_gate_replay` (the gate's
  own C code over the same objects as the app; no model) replays the anchor
  gate over every claim the live runs recorded (the review sheets' claim
  answers, questions, verified spans and the fixture's author turns) and sets
  the current verdict beside the recorded one; a gate change is measured this
  way, and against the committed gate's binary, before it ships.
  `test_answer_gate_replay` (in `make test`) pins the claim extraction.
  The same replay runs the quotation check (`answer_quotations`: every
  quotation in a composed reply must be a substring of a verified span, an
  author turn or the question under the gate's normalization) over every
  recorded reply and lists the pieces the spans do not contain;
  `test_answer_pipeline` pins the extraction (straight and curly pairs that
  close on their own kind, blockquote lines, ellipsis and newline pieces,
  emphasis and punctuation folding, mentions under the minimum span length,
  unterminated marks) and the withheld reply's reason.

- `test_recall` (in `make dbtest`) drives `recall_run` against real SQL in a
  throwaway database: displayed canon text is byte-equal to the current
  committed version; retracted canon, unmatched canon and another project's
  canon are excluded; coverage counts (committed, retracted, conversations with
  memory on, off and indexing, candidates, rejected) are exact; a changed
  original message or saved revision removes its excerpt; revoking consent
  removes a conversation's excerpts while project-wide canon stays; file-backed
  projects report that excerpts need the store; empty and oversized queries
  fail closed; row caps are enforced; and the background job hands over one
  result per run. The `recall_terms` case in `test_core` covers splitting,
  lowering, deduplication, bounds and substring matching.
- `python3 tools/test_recall_mutations.py` weakens each host-only guard in
  `recall.c` (status filter, canon term match, ranking, canon cap, no-terms
  guard, query bound), relinks `test_recall` and expects it to fail. Guards that
  SQL also enforces are deliberately not listed. Needs the services up.
- `tests/ui/store/15_recall.scribo` (in `make uitest-db`) commits canon from a
  selection, searches before and after enabling a chat's memory, checks the
  coverage counts and row order, opens a canon row's panel, selects an excerpt's
  source message, and checks that a no-match query reports bounded coverage.

### Reach measurement (MEMORY-015 S2)

- `tools/memory_reach_eval.sh [--words N] [--per-category K] [--export-native FILE]`
  builds a deterministic manuscript-scale chat archive (`tools/memory_fixture.py`:
  shared characters, places, objects and colours everywhere, with planted items
  in nine labelled categories), ingests and indexes it through the production
  path with the real tokenizer and embedder, then runs the production planner
  and admission for every task with no generation. It reports, per category,
  how often the required source turns were among the candidates and among the
  admitted excerpts, which discovery path reached them, how many planted
  near-misses were admitted, and planning time against the 500 ms budget. Reach
  is a measurement; the harness gates only its own validity (control category
  at 1.0, both discovery paths available, ingestion complete, zero privacy
  violations, namespaces cleaned). `--self-test` checks the fixture alone.
- `tools/lexical_bench.py [--words N] [--per-category K] [--collation C]`
  isolates the lexical channel: it loads a fixture's archive into a scratch
  table with the production full-text index, runs every task's question under
  the production score and length-normalized alternatives, and reports hit
  rate and rank per category. It never rebuilds the index (see MEMORY-018 on
  why) and drops its database afterwards.
- `tools/manuscript_baseline.sh [--words N]` seeds a manuscript-scale legacy
  project from the fixture (`tests/seed_manuscript.py`: the chat archive plus
  eight chapter documents), opens it in the real app with the fake model, and
  reports one `FRAME_METRICS` line per phase (store import, chat open, paging,
  document edit and save, send) plus sampled RSS (`tests/ui/store/16_manuscript_baseline.scribo`).
- `python3 tools/memory_native_eval.py FILE` now accepts the exported episodes
  so the model's use of admitted sources is measured on the same fixture;
  `tools/memory_use_score.py EVENTS ANSWER_KEY` gives a coarse alias/openness
  screen per category, presentation and layout. The frozen review rubric
  remains the standard for any quality claim. Runs are retained under
  `build/memory-reach/runs/` and `build/mn-*`.

### Persistence safety regressions

- `test_llm: concurrent_file_readers` loads separate chats and revision indexes
  simultaneously, checking counts and ownership of every entry. It also runs
  under ASan/UBSan and TSan, including malformed trailing JSON. Application JSON
  parsing uses `src/core/json.c`: cJSON's shared error state must never be written
  concurrently, even when callers don't use its error-pointer API.
- `test_db: transition_read_failures` checks that invalid or unreadable chats
  abort migration without switching authority or committing partial chats. After
  repair, migration can be retried. Decode failures for notes, canon and findings
  must fail export rather than produce a falsely empty backup. Reusing a partial
  or completed export destination is refused, and chat flags/timestamps survive
  the store-to-file export.
- `test_llm: chat_store_read_failures` checks all-or-nothing failure for malformed
  recognized messages, wrong-type timestamps, raw/escaped NULs and special files;
  literal escaped backslashes and missing legacy timestamps remain supported.
  Truncation flags and fractional timestamps round-trip, and a later rename still
  follows a fractional-time source.
- `tests/test_backup.py` runs in `make test` without Docker or a display. It checks
  successful verification, symlink/special-file refusal, destination safety,
  cleanup after verification failure, and complete checksums for unusual names
  and author-owned files named `SHA256SUMS`. Unreadable extra directories fail
  verification instead of being skipped. See the dated validation and mutation
  evidence in [`REDTEAM-010-persistence.md`](REDTEAM-010-persistence.md).
- `test_llm: file_history_read_failures` checks unreadable record logs and revision
  indexes, unchanged data after refused writes, and clean empty-history results.
- `test_db: legacy_import_read_failures`, `fork_retry_snapshot`, and
  `import_revision_identity` cover rollback/repair, immutable fork retries across
  store restart, copied pending-state ownership, post-commit journal cleanup, and
  equal snapshot filenames in different documents.
- `test_persistence_faults` is a separate binary that fails each instrumented
  allocation in representative record/revision/chat reads and writes, in both
  one-shot and sustained-failure modes. It runs in
  `make test` and `make sanitize`; injection is not compiled into the app. The
  cJSON implementation is compiled through `core/json_vendor.c` so parse OOM can
  be distinguished from malformed crash-torn input without switching allocator
  hooks at runtime or modifying the pinned dependency.

The follow-up evidence and recovery contracts are in
[`REDTEAM-011-legacy-fork.md`](REDTEAM-011-legacy-fork.md). With the pinned weights
already cached, `make llama` builds the runtime without unused web assets; if
CMake is available only through a cached uv tool, use
`make CMAKE="uvx --offline --from cmake cmake" llama`. Model weights are acquired
separately by `make model`; acceptance never needs to select a quantized fallback.

[`REDTEAM-012-marker-snapshots.md`](REDTEAM-012-marker-snapshots.md) records the
retained-copy-marker regression, the 1,000-name snapshot exhaustion test, and the
non-monotonic-clock export/import check.
`snapshot_reuse_integrity` covers missing/damaged candidates, repair, and missing
older-source snapshots; reuse cannot succeed from an index hash alone.

## Automated trusted acceptance

The repository-scoped `scribodo-mac-studio` runner executes database and UI
acceptance only for repository-owner events; fork pull-request code is never sent
to it. Each job uses a disposable `scribodo-ci` Compose project, temporary secrets,
ports 33062/6334 and always-run volume cleanup. Manual/scheduled runs additionally
exercise `make backuptest remotetest`. Adding a write collaborator requires review
of this owner-only trust policy first.

## Hang protection

Two layers make sure we never wait on a hung process:

1. **`tools/watchdog.sh SECONDS NAME CMD…`** wraps every test target. It holds the machine awake for the command's lifetime (`caffeinate -i -w`). If the command outlives its limit, the watchdog samples every thread of it and its children into `build/hangs/NAME-<time>.txt`, appends the Mac's sleep and wake events since launch, prints the main thread's innermost frames, kills it, and exits 124. A real example from the chat work:

   ```
   WATCHDOG: 'app-deadlock' still running after 8s; sampling to build/hangs/app-deadlock-093626.txt
       main  (in scribodo)  main.c:752
       chat_view_update  (in scribodo)  chat_view.c:144
       chat_version  (in scribodo)  chat.c:127
       __psynch_mutexwait  (in libsystem_kernel.dylib)
   ```

2. **The script runner's heartbeat.** In script mode, a watchdog thread checks that the main loop keeps producing frames. After 15 s with none, it reports the script line being executed as a failure and aborts, which leaves a crash report with every thread's stack. That re-planted deadlock failed in 16 s; by hand it had taken 10 minutes to notice.

Limits: unit suites 300 s each, UI scripts 120 s each, `llmtest` 600 s, real-model UI scripts 600 s.

## UI tests (`make uitest`)

Each script in `tests/ui/*.scribo` launches `build/scribodo` in a **hidden window**, so it never steals focus and your real mouse has no effect on it. The script gets a fresh copy of `tests/ui/fixtures/` in `build/ui/<script>/work/`, and its screenshots land in `build/ui/<script>/`. The app exits non-zero if any expectation fails.

How input is driven:
- **Keys, modifiers, mouse movement and buttons, the scroll wheel, window close and resize** are injected through raylib's own event playback (`PlayAutomationEvent`) at the start of each frame. The app's normal input handlers (`handle_keyboard`, `handle_mouse`, splitter dragging, the close guard) run unmodified.
- **Typed characters** go through `input_inject_chars()`. raylib's playback can't create character events, and in the real app those come from the OS text system.
- **The clipboard** is private while a script runs, so tests never read or overwrite yours.
- **Shortcuts** match physical key positions in scripts. That keeps results the same on any keyboard layout; real use matches by layout (see `src/ui/input.h`).

The command reference is at the top of `src/app/script.h`. A minimal example:

```
open "$DIR/chapter.md"
click_index 5 2            # double-click inside "fog"
expect selection 4 7
key C cmd
expect clipboard "fog"
key END cmd
type " The end."
key S cmd
expect dirty no
shot saved                 # build/ui/<script>/saved.png
```

Scripts click on text by buffer index (`click_index`, `drag_index`), not by pixel, so they don't depend on font metrics. Expectations that depend on where a line wraps use ranges (`cursor_range`, `length`).

Element lookups (`click_element`, `expect element_w`, `expect element_y`)
scroll an explorer item that is laid out outside the panel's visible area
into view first, and only an explorer descendant: `expect element_y NAME MIN
MAX` reports where a click on the element would land, which is how
`tests/ui/store/19_explorer_scroll.scribo` checks that a toolbar button's
rectangle does not move when the explorer is scrolled ([REDTEAM-016](REDTEAM-016-real-manuscript.md)).

### Running with the display asleep

Creating a window needs an awake display. If the Mac's display is asleep, the app exits with `could not create a window`. Keep it awake for the run with:

```sh
caffeinate -d -u -i make uitest
```

### What the UI tests can't cover

These sit above raylib and need a real-input pass:
- dead keys and ⌥-symbols (composed by macOS before raylib sees them)
- input-method composition
- layout-aware shortcuts on non-US keyboards
- the real system clipboard
- key auto-repeat
- file drops from Finder
- HiDPI changes when moving between displays

## Checking that the tests catch regressions

A passing suite is only meaningful if it fails when behavior breaks. For the key safety and UI behaviors, we reverted each one in a scratch copy of the tree, rebuilt, and confirmed the suite fails. See [`REDTEAM-001-editor-spike.md`](REDTEAM-001-editor-spike.md) for the editor-level results. For the UI suite:

| Behavior removed | Caught by |
|---|---|
| Unsaved-changes close guard | `close.scribo` |
| Discarding keystrokes typed while unfocused | `mouse.scribo` |
| Per-frame pane width clamp | `panes.scribo` |
| Double-click word selection | `mouse.scribo`, `clipboard.scribo` |
| Drag auto-scroll | `scroll.scribo`, but only with *both* scroll paths removed (see below) |

Two lessons from building the harness:
- **Rebuild cleanly between mutations.** macOS's bundled GNU Make 3.81 compares timestamps at one-second resolution, so a file restored within the same second as its last build can leave a stale object behind. The mutation harness waits more than a second before touching files, and checks that the binary actually changed.
- **Treat crashes as errors, not passes or catches.** One mutation run "missed" everything because the display was asleep and every launch aborted. The harness now voids any run where a script didn't report a summary.

The chat feature was red-teamed separately; see [`REDTEAM-003-chat.md`](REDTEAM-003-chat.md) (19 findings, all fixed or documented, 8/8 fixes mutation-verified).

## Reviewed AI edits

- **Unit tests:** `test_core: diff` (word-level Myers diff, with a 300-case reconstruction property and fallback at the edit limit) and `test_llm` (edit-reply cleaning, the edit request, fake-model edits, revision store integrity). `test_editor: ranges` covers range read, replace and find; `test_editor: anchor` covers the tracked passage through edits, undo and redo.
- **`edits.scribo`** (fake model) covers:
  - select, ⌘K, review, Accept and the revisions it records;
  - restoring from History, then undoing the restore;
  - Reject;
  - a stale passage refused, and a moved passage re-anchored;
  - Esc out of edit mode;
  - a revision recorded on save;
  - read-only documents refusing edits;
  - the red-team 004 regressions: ⌘↩ typing nothing, the wrong-copy and mid-word cases, stopped replies, a failed "before" revision.
- **`real-edit.scribo`:** a real LFM3 rewrite reviewed and accepted through the UI.
- **Mutation check:** four safety guarantees, each broken in turn: applying without Accept, Reject applying anyway, applying over a stale passage, and skipping the "before" revision. All four were caught. The first run "missed" the most important one, because the mutated source didn't compile and the harness re-ran the old binary. Mutation harnesses must check the build and that the binary changed.
- **Red-team:** see [`REDTEAM-004-edits.md`](REDTEAM-004-edits.md) (17 findings, all fixed; 18 of 20 mutations caught, both misses documented).
- **ASan caught an out-of-bounds read** in the diff tokenizer: `memcmp` against the punctuation table near the end of the text. It's fixed by bounding the comparison.

## Notes and canon

- **Unit tests:** `test_llm: records` covers:
  - versions numbered per entry, with every version kept, most recently changed first;
  - document and chat sources round-tripped;
  - snapshots reused when the text is unchanged;
  - a torn line skipped, and the next record starting on its own line;
  - ids and source names that look like paths refused;
  - hand-edited lines with missing numbers.
- **`notes.scribo`** covers:
  - a selection saved as a note with its exact source;
  - editing (⌘S, and saving on Close);
  - opening the source, including after the passage moved;
  - a canon draft that stores nothing until Commit, and needs a second Close to discard;
  - Revise, history, Retract and Recommit;
  - chat replies as notes and canon drafts;
  - deleting, and blank notes that are never stored.
- **Mutation check:** 9 of 9 caught:
  - canon discarded without confirmation;
  - note edits lost on Close;
  - canon stored when a draft opens;
  - deleted notes still listed;
  - a moved source not found;
  - versions always 1;
  - a torn line not terminated;
  - source paths trusted;
  - snapshots not reused.
  A tenth, invalid mutation (it didn't compile) was voided by the harness and rewritten.
- **UBSan caught a real bug:** missing JSON numbers (a document source has no `message` field) read as NaN and were cast to integers. The revision index had the same latent bug. Both now use a checked reader.

## Observer

See [`OBSERVER-001-design.md`](OBSERVER-001-design.md):
- unit tests for evidence matching and an end-to-end fake-backend run;
- `observer.scribo` and `real-observer.scribo`;
- ASan/UBSan and the new `make tsan`, all clean;
- 10 of 10 mutations caught.

Red-teamed in [`REDTEAM-005-observer.md`](REDTEAM-005-observer.md): 16 findings, all fixed, 20 of 20 mutations caught.

UI scripts start with the observer **off**, so other scripts' file expectations stay deterministic. A script turns it on with `observer on` and waits with `wait_observer`.

## Projects and documents

- **`projects.scribo`** and **`projects_rt.scribo`** cover:
  - the welcome screen;
  - opening projects;
  - New document, Import, and Rename via the context menu;
  - modal input;
  - sources across renames.
- **`make cstest`** runs `case_sensitive.scribo` on a temporary case-sensitive APFS image.
- Red-teamed in [`REDTEAM-006-projects.md`](REDTEAM-006-projects.md): 13 findings, all fixed, 13 of 13 mutations caught.

## Chat attachments

- **Coverage:**
  - `attachments.scribo` and `attach_many.scribo`;
  - `test_llm: multi_doc_request`: fair budget sharing, cut marking, neutralization, and protection against forged framing.
- **Red-team:** [`REDTEAM-007-attachments.md`](REDTEAM-007-attachments.md): 12 findings, all fixed; 9 of 9 mutations caught.

## Project store

**`make dbtest`** runs against the local MariaDB in throwaway databases. It covers:
- migrations, including 003 applied to a database that already has data;
- prepared statements, with hostile text and a 3 MB value;
- NULLs, foreign keys, rollback and cascades;
- stable ids and byte-exact names (trailing spaces included);
- reconnecting after the server drops the connection;
- the revision, record and chat backends: History in recorded order whatever the timestamps, and sources never linked across projects;
- the importer: revisions, notes, canon and chats, a rename made before the import, idempotence, legacy files kept, and paused mode;
- import safety: one importer at a time, a stale project row not reused, a copied folder detected and forked into its own project, a folder moved across volumes kept as the project, and a project missing from the store restored from its saved files;
- the entity graph: name normalisation, mentions and links from findings, an unnamed end left unlinked, dismissal, idempotent backfill, and no links across projects;
- semantic retrieval against Qdrant with the fake embedder.

It's also clean under ASan/UBSan.

**`make uitest-db`** runs the app six times on one database (`tools/services.sh up` first):
1. A project with existing files is moved into the store, then notes, canon, a chat, a rename and the observer are used.
2. After a restart everything comes back, the chat resumes (SOW acceptance 1), and the observer reads nothing twice.
3. The project is backed up and restored into a separate folder; records, chats, attachments and History survive.
4. The entity graph (`5_entities.scribo`): the fake observer's relationship finding ("Zoë is Mara's sister.") becomes two entities and a link. "›" goes to the other entity, and the relationship opens its passage.
5. A copy of the folder (`cp -R`) is recognised as a copy and paused (chat refuses). "Make it its own project" brings over the original's documents, chats and canon under a new project.
6. With the database unreachable, documents stay editable and saveable. Chat, rename and New pause, and nothing falls back to files.

## Findings from the chat work

Each of these was caught by the suites and has a regression check:

- **Self-deadlock in the chat view.** It took the chat lock and then called a function that takes the same lock, freezing the first frame the chat pane drew. Fixed; this is the bug the hang protection exists for.
- **Typing into the chat input never worked.** The unfocused document editor drained the shared typed-character queue before the chat input read it. The drain now happens once per frame at the app level, after every text view has updated.
- **ID collision.** The Explorer's first chat row (`CLAY_IDI("Chat", 0)`) hashed identically to the chat pane (`CLAY_ID("Chat")`), so clicking anywhere in the chat pane opened a saved chat. Rows are now `ChatItem`/`DocItem`, and `chat.scribo` clicks the pane and checks that nothing opens.
- **Stale element boxes.** `Clay_GetElementData()` returns the last known box of an element that's no longer declared (a closed pane), which made a closed Observer look open. `clay_element_current()` checks Clay's per-layout generation before any box is trusted.
- **Pane clamping at minimum width** could squeeze the document to ~155 px. It now shrinks chat, then the Observer, then the Explorer, and finally closes the Observer.
- **raygui renders with the primary font face only,** with no fallback. Button labels must avoid glyphs Verdana lacks; the Observer button showed "?".

## Findings from the UI suite

- **The unsaved-changes dot rendered as a missing-glyph box.** "●" (U+25CF) and "⌘" (U+2318) were outside the character ranges we load. Fixed by loading the Geometric Shapes and Miscellaneous Technical blocks. Found by screenshot, not by an expectation.
- **The app aborted inside GLFW when no display was available.** It now checks `IsWindowReady()` and exits with a clear message.
- **Drag auto-scroll has two mechanisms:** explicit edge scrolling, and keep-the-caret-visible scrolling that fires on every drag update. Either one alone auto-scrolls; together they scroll faster. This isn't a bug, but worth knowing before tuning scroll speed.
- **Harness bug:** screenshots taken just before `EndDrawing` missed the last draw batch (the Save button). The capture now flushes raylib's batch first.

## Offline, backup and remote

- **`make offlinetest`** (acceptance 8): everything inside a macOS sandbox with no network but this machine (`tools/offline/no-network.sb`), with controls proving the sandbox blocks outside hosts. It covers the store suite and the real models (chat, edit, the observer on files and on the store, `llmtest`). It needs the real model to itself.
- **`make backuptest`:** a disaster rehearsal, plus crafted backups that must be refused:
  - planted files;
  - links;
  - a shell escape;
  - another database;
  - a file that isn't a dump;
  - a missing `--to`.

  A `--replace` must also drop the stale index.
- **`make remotetest`:** the remote model against a stand-in provider. It checks what actually reached the provider: one request, with its key and document, nothing from before the switch, and nothing after switching back.

## The trusted runner is this Mac, and it sleeps

`pmset -g` on the development Mac sets system sleep to one minute on AC once
the display is off and no process holds a sleep assertion. On 2026-11-07 a
native model run and a trusted acceptance job both lost 942 seconds to idle
sleep that way (MEMORY-017). The acceptance workflow steps and the three
experiment runners (`memory_native_eval.py`, `memory_reach_eval.py`,
`memory_quality_eval.py`) therefore run under `caffeinate -i`. Anything else
long and unattended should too. Note the clocks: `CLOCK_MONOTONIC` keeps
counting through sleep on macOS, Python's `time.monotonic()` does not, and
bash `$SECONDS` is wall time; a run that straddles sleep reports different
durations from each.

The same thing happened to the unit suite on 2026-11-08 and was first
misread as a deadlock: `make test` and `make tsan` both reported `test_llm`
killed by the watchdog after 300 s, with a stack sample showing the observer
test in a plain `usleep`. `pmset -g log` placed the launch at 11:35:32 inside
a 46-second dark wake, the next sleep at 11:35:39 and the sample at 11:53:09,
the second the Mac woke again; the test had run for seven seconds. Run alone,
`test_llm` passes in well under its limit. Since then `tools/watchdog.sh`
holds a `caffeinate -i -w <pid>` assertion for every command it wraps, so a
unit test can no longer sleep into its own limit, and every hang sample ends
with the machine's sleep and wake events since the command was launched, so
the next reader can tell a hang from a nap without consulting `pmset`.

A legacy project's move into the store runs on an owned worker since
MEMORY-019 (`src/app/import_job.c`); UI scripts that open a freshly seeded
legacy project must `wait_status "now kept in the project store"` before
using it. `tools/import_probe.c` times that import and counts its SQL
statements on a seeded project.

A real manuscript goes through the same harnesses (MEMORY-021):
`tools/manuscript_import.py SOURCE.txt DEST --parts 8` turns a Project
Gutenberg text into chapter files and eight attachable parts;
`tools/memory_reach_eval.sh --manuscript DEST/parts --manuscript-tasks KEYS.json`
measures selection reach against hand-written answer keys
(`tests/fixtures/memory_reach/pride-and-prejudice-tasks.json`, and the held-out
`a-princess-of-mars-tasks.json` for the second novel);
`tools/manuscript_baseline.sh --manuscript DEST/parts --script
tests/ui/store/18_manuscript_questions.scribo [--real]` asks the app
questions about it. `--keep` leaves the disposable database and work
directory for diagnosis, and `tools/services.sh testdb sql NAME` runs SQL
from stdin against a test database.
`tools/manuscript_baseline.sh --manuscript DEST --script
tests/ui/store/19_explorer_scroll.scribo` runs the explorer scroll-into-view
check on the 61 chapter files, where the chat list is laid out off screen.
The same runner drives the two preparation probes from MEMORY-022:
`tests/ui/store/19_manuscript_prepare.scribo` (indexing may still be warming)
and `tests/ui/store/20_manuscript_ready_prepare.scribo` (complete-index gate).
Both cancel generation after checking the as-of source packet; they
establish preparation, not answer correctness.

`tools/chat_hang_probe.c` (build it with the overlay in its header comment)
drives a real `ChatSession` against a stand-in server that accepts the
completion and never answers, with or without stream headers, requests
cancellation and samples the process. Both modes end within 30 ms of
cancellation with the reply marked "Stopped".

## Where cited evidence lives

Dated reports cite `build/` run directories and logs. Those are archived, not
deleted, when the build folder is cleaned; [`EVIDENCE.md`](EVIDENCE.md) maps the
cited paths to the dated tarballs.

## Test-harness environment variables

These exist for the scripted suites and debugging. Operator-facing variables
are listed in the README.

| Variable | Effect |
|---|---|
| `SCRIBODO_SCRIPT` | Path to a `.scribo` UI script. The app runs it in a hidden window, uses the fake model unless `SCRIBODO_LLM=real`, and uses files unless `SCRIBODO_STORE=db`. |
| `SCRIBODO_SCRIPT_DIR` | Working folder for a scripted run: its `profile.json`, `model-settings.json` and `.scribodo-state` live here instead of Application Support. |
| `SCRIBODO_SHOT_DIR` | Where a script's screenshots are written. |
| `SCRIBODO_LLM` | `fake` forces the deterministic stand-in model; `real` forces the real `llama-server` inside a script. |
| `SCRIBODO_SEED` | Sampling seed sent to the local model for reproducible real-model runs. Ignored for remote providers. |
| `SCRIBODO_DB_NAME` | Throwaway database name for `dbtest`, `uitest-db` and the acceptance runner. |
| `SCRIBODO_BACKUP_TIMEOUT_SECONDS` | Shortens the one-hour backup run timeout. It can only shorten it. |
| `SCRIBODO_SCREENSHOT` | `out.png` renders a few frames, saves the framebuffer and exits. |
| `SCRIBODO_INPUT_TRACE` | Logs decoded input characters to stderr. |
| `SCRIBODO_SEM_DEBUG` | Logs failed Qdrant key reads to stderr. |
