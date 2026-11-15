# AUDIT-004: technical-debt audit after the native-services and observer cycles

**Date:** 2026-11-15
**Baseline:** `3731db8` (`Observer steps 8 and 9: hosted verdicts`)
**Previous audit:** [AUDIT-003](AUDIT-003-repository-audit.md) at `bd23f8b`; 84 commits have landed since.
**Scope:** the whole tree, by AUDIT-003's method: every verification target sequentially on an unchanged tree,
four read-only reviews by area (`src/app` with the services layer; `src/llm`; `src/main.c` with `src/ui` and
`src/core`; tooling, tests, docs and CI), every standing claim of the [register](TECHNICAL-DEBT.md) re-checked
against HEAD, and repository hygiene. Debt means what makes the next change expensive or the next claim
unreliable; defects found on the way are recorded as defects. Each finding below was re-checked by hand against
the tree after the reviews (file and line as of `3731db8`).

Logs are under the untracked `build/audit004/` and are archived (see [EVIDENCE](EVIDENCE.md)).

## What landed since AUDIT-003

| Area | Files | Lines added | Lines removed |
|---|---:|---:|---:|
| `src/` | 56 | 3,486 | 1,257 |
| `tools/` and `tests/` | 106 | 12,050 | 229 |
| `docs/` and `journal/` | 51 | 7,146 | 47 |
| whole tree | 215 | 22,559 | 1,311 |

The shape AUDIT-003 named holds and has sharpened: 15 % of the added lines are product code, and the product
code itself removed a third of what it added (the index-text retirement, the builder split). Instruments and
prose still accrue without retirement, and this time the cost has arrived: evidence the journals cite is being
lost (F1), and two measured claims rest on agreements nothing re-checks (F2).

## Verification matrix (unchanged tree)

Every C and Objective-C source was touched so that `all` recompiled it with warnings visible. Targets ran one
after another on battery, the machine otherwise idle; wall times are in `build/audit004/results.tsv`, output in
`build/audit004/<target>.log`. **Eighteen of nineteen targets exited 0 in 1,486 s** of sequential wall time.

| Target | Result |
|---|---|
| `all` | every source recompiled, **0 compiler warnings** |
| `analyzetest` | 8 Python gate tests passed |
| `test` | **95,807 C checks** and **200 Python tests**, 0 failures |
| `analyze` | 0 warnings across 87 C/Objective-C files |
| `dbtest` | **18,006 checks** in 12 binaries, 0 failures |
| `crashtest` | 1,478 checks, 0 failures; 133 matrix rows (495 s) |
| `sanitize` | the same 95,807 checks and 200 Python tests, 0 failures; 0 ASan/UBSan diagnostics |
| `tsan` | **48,352 checks** (the sum of the "N checks" lines), 0 failures; 0 ThreadSanitizer diagnostics |
| `uitest` | 409 passed, 0 failed (23 scripts) |
| `uitest-db` | 304 passed, 0 failed (16 scripts) |
| `backuptest` | 38 UI assertions and 17 checks, 0 failed |
| `remotetest` | 14 passed, 0 failed |
| `cstest` | 3 passed, 0 failed |
| `servicestest` | steps 1 to 9 ok; step 10 (Docker migration) skipped, Docker not running (F6) |
| `dependency-report` | **FAILED**: `mariadb:11.4.13` now resolves to `sha256:5e73baf8…`, not the pinned `sha256:d1891ede…` (F13); every other pin verified, no OSV advisories |
| `llmtest` | 24 real-model checks, 0 failures |
| `uitest-real` | 35 passed, 0 failed (3 scripts) |
| `offlinetest` | script passes and 24 real-model checks, 0 failed |
| `tools/test_audit_mutations.py` | 10 mutants killed, 0 survived; both controls passed |

Every count is at or above AUDIT-003's except `tsan` (48,488 to 48,352); its binary list is unchanged, and this
audit did not trace the 136-check difference.

## Findings

### F1 — evidence archiving lapsed and cited evidence is already lost (P1, debt; the loss is a defect)

The last archive is `scribodo-evidence-2026-11-12-loose-ends` (2026-11-12). A scan of every `build/` path cited in
`docs/`, `journal/` and the README against the fourteen archive manifests finds 433 cited paths: **106 exist and
are in no archive** (about 2.2 GB; the observer cycle's six-hour real-model runs, the GAP-003 series, the native
services move, REDTEAM-023) and **35 exist nowhere**. Most of the 35 are pre-archive history or make's own
directories named in prose (`build/unit`, `build/headless`, `build/test/DB`), but some are recent and real:

- `build/ui/fit-on` (`journal/answer-cycle4_default_path.md:95`, the three-server launch footprint) and
  `build/ui/fit-native2` (`journal/store-cycle2_native_services.md:92`, the 32K-context fit) were written into
  a directory that `make uitest` deletes on every run (`Makefile:330`, `rm -rf build/ui`) and that the acceptance
  runner runs on every push.
- `build/gap003/{gates,frames/gates,orphan/gates,reap/gates,soak-smoke}` and `build/recover/gates2` are gone.

The README (`:242`) cites unarchived `build/observer/redteam023/durability/` as living guidance. One `make
distclean` would erase three days of observer and GAP-003 evidence. The rule ("archive at every dated report")
exists; nothing enforces it, and the evidence path a gate wipes is not distinguished from one it keeps.

**Cost:** the 32K-context and three-server fit claims can no longer be checked; the next lost directory will be
noticed only when someone tries to re-check a claim.

### F2 — measured observer claims rest on C/Python agreement nothing re-checks, and one flag now names two arms (P1, debt)

Three C functions are ports of the Python rules they were measured as: `obs_sentence_end`
(`src/llm/observer_text.c:375`, of `tools/observer_plant.py` and `observer_sentence_pairs.BOUNDARY`),
`obs_attributes_differ` (`:527`, of `a_any`) and `obs_same_subject` (`:597`, of `subjects`) in
`tools/observer_judge_score.py`. Agreement was measured once each by scratch probes (0 of 83,149, 0 of 5,226,
0 of 67,943; `journal/observer-cycle2_quality.md` steps 8 and 9); the probes are not tracked and no test reads a
shared fixture. `test_observer_text.c` and `test_observer_judge_score.py` use different hand-picked cases. Known
divergences a shared fixture would catch: the C sets cap at 64 words and skip words of 64 bytes or more
(`ATTR_MAX`/`ATTR_LEN`, `:414-423`), Python has no cap; and `observer_plant.py:28` says `obs_sentence_end` "is
the same rule" as `ends_sentence`, which it is not (the C also requires the following capital, which is
`BOUNDARY`'s rule). The relation check shows the right pattern already: `tests/fixtures/answer_relation/replies.json`
is read by both `test_answer_pipeline.c` and `test_answer_relation_arm.py`.

Second, step 9 (`12ed760`) put the same-subject co-signer under the flag step 8 had measured
(`SCRIBODO_OBSERVER_SENTENCES`; `observer.c:736-740`). Step 8's arm (attributes only, 0.14 and 0.28) can no longer
be selected from the product, which breaks "delete nothing, only archive" (MEMORY-015 §5), and the field comment
(`src/core/experiments.h:36-38`) still says "steps 7 and 8; default off until measured". This one is the audit's
own author's: it landed today.

**Cost:** the precision 0.80/0.71 claim lasts only until either side is edited; step 8 cannot be reproduced.

### F3 — `main.c` keeps absorbing workflows; `main()` is 947 lines (P1, debt; register entry drifting)

`src/main.c` is 4,703 lines (was 4,555; 11.2 % of `src/`), 297 short of the register's 5,000-line split trigger,
at about 37 lines a day. The growth is remediation placed straight into the composition root: the unsaved-draft
workflow (`draft_tick`/`draft_discard`, `:1657-1701`, four App fields), `resave_unsaved_chat` (`:1859-1888`),
the reranker-missing check and orphan reaping inside `main()` (`:3863-3879`), a fixture redirect, a shortcut and
a host adapter. `main()` itself (`:3757-4703`) is the longest function in the tree and is on no register row,
nor is `build_layout` (247). The P1 entry's criterion ("composition root rather than the implementation of every
workflow") is receding. Related, lower: the four App unions mirror their controllers exactly today with no
`_Static_assert` on layout (`:163,177,196,289`), and `ScriptHost` is initialised positionally with 32 members of
two repeated signatures (`:4010-4016`), so a misplaced field or callback compiles.

### F4 — experiment arms are not pinned where measurements are taken (P2, debt)

Since `52d44fe`, `SCRIBODO_ANSWER_PIPELINE` is on unless set to `0`/`off` (`src/core/experiments.c:18-22`).
`tools/manuscript_baseline.sh:72` and `tools/manuscript_restart.sh:72` set `SCRIBODO_MEMORY_EXPERIMENT=1` and
nothing for the pipeline, so the same command line now starts the reranker (memory taken from the chat context,
10 to 15 s a reply) where before 2026-11-14 it did not. GAP-003 (P0) baselines on either side of the flip are not
comparable unless the arm is read from the run. Likewise the observer flags: `observer_conflict_gate` changes
`tokens()` (`observer_text.c:248`) and so `obs_overlap`, whose three callers include note linking and sentence
pairing, not only the conflict path; of the four on/off combinations of the two observer flags, one is covered by
the suite, one by a single fake-backend test (`test_llm.c:1518`), one only through `obs_overlap`, and both-on by
nothing. `tools/observer_replay_run.sh` sets neither.

### F5 — a user-facing measured figure is tied to no arm (P2, debt)

`src/ui/findings_view.c:180` shows "about 1 in 13 was real on the test novels" beside every possible conflict.
That is the default path's figure (journal step 4, 7 in 92). The view does not consult the flags, under which the
measured figure is 0.14 to 0.80 depending on arm. Promoting the flag, or re-measuring the default, leaves a stale
claim in front of the author with nothing to catch it.

### F6 — the services layer: reasons discarded, defaults written four times, the Docker path untested (P2, debt)

- `service_supervisor.c:31-33` sends `tools/services.sh up`'s output to `/dev/null` and keeps a boolean
  (`:52`; `main.c:4083`). The script now refuses for reasons the author must act on (Docker-owned credentials,
  `return 2`, `services.sh:177`; MariaDB not 11.4, `:99`; Qdrant not built, `:95`; port taken, `:186`); in the
  app all are one "store offline". No test pins the exit-code contract.
- Ports 33061 and 6333 are written in `services_lib.sh:19-20`, `docker/compose.yml:24,43`, `db.c:50` and
  `semantic.c:65`; the secrets directory in four files (`services_lib.sh:13`, `db.c:58`, `semantic.c:71`,
  `remote.c:26`); the MariaDB server flags twice (`compose.yml:12-16`, `services.sh:103-107`).
- Acceptance pins `SCRIBODO_SERVICES: native` (`.github/workflows/acceptance.yml:31`) and the rehearsal skips the
  Docker migration step without Docker (`servicestest.sh:241`), so Docker-mode `rootsql`, `dump_db` and
  `load_dump` (`services_lib.sh:63-118`, each written twice) run in no CI lane. The two Qdrant calls authenticate
  differently (`api-key:` at `services.sh:207`, `Authorization: Bearer` at `services_lib.sh:120`).
- `services.sh` grew from 77 to 414 lines; `migrate-from-docker` is about 115 lines inline in the `case`.
- The app finds the script relative to its binary (`main.c:799`, `../tools`), which no `.app` bundle has (GAP-002).
- Six of the script's variables are documented nowhere (`SCRIBODO_COMPOSE_PROJECT`, `_LOGS`, `_MARIADB_BIN`,
  `_QDRANT_BIN`, `_MARIADB_MEM_LIMIT`/`_QDRANT_MEM_LIMIT`, `_SERVICES_LOCKED`).

### F7 — two more unversioned C-to-Python contracts (P2, debt)

The judge log (`judge_log_line`, `observer.c:696-723`, read by `observer_judge_score.py`) and the replay's
twelve-field escaped finding lines (`tools/observer_replay.c`, parsed by `tools/observer_lines.py`) are each tested
on one side only (`test_llm.c:1463-1510` reads some judge-log fields; the Python tests build their own lines). No
fixture is written by C and read by Python. This is AUDIT-003 F4's shape before its remediation, and the line
format has already changed once (escaping, 2026-11-13). Defect on the way: `test_llm.c:1498-1502` and `:1568-1569` dereferences
`cJSON_GetObjectItem(...)->valuestring` with no null check.

### F8 — standing documents contradict the register and each other (P2, debt)

- `docs/LANDSCAPE.md` row 21 lists as open four gaps the register closed on 2026-11-12 (migration 015's column,
  the 308-line chat builder, seven atomic writers, undeclared Python environments); row 20 says the third novel
  has no reach tasks (`tests/fixtures/memory_reach/wuthering-heights-tasks.json` exists and has run); "Reading
  the table" calls window construction the first open item (done 2026-11-11 per PRODUCT-GAPS).
- `docs/PRODUCT-GAPS.md`'s header still says "re-reviewed 2026-11-09 … no gap status changed"; GAP-004's status
  gives 129 rows and 125 deaths where the matrix has 133 and 129, and its body still says "next work: add
  deterministic kill points".
- The register says "Last reviewed: 2026-11-11" above edits of 11-12 and 11-14; its CI section
  (`TECHNICAL-DEBT.md:494-505`) says Docker Desktop supplies the services; its experiment entry says "eleven
  fields" (there are 14 reading 12 variables) and omits `SCRIBODO_OBSERVER_SENTENCES`, which is also missing from
  the README table (as is `SCRIBODO_SEM_DEBUG`).

### F9 — tools no gate compiles, and a yardstick that lives in prose (P2, debt)

`make tools` runs in no workflow (`ci.yml`, `acceptance.yml`), so `observer_replay`, `observer_judge_pairs` and
`observer_overlap` can stop compiling unnoticed; six more C tools build only through Makefile overlays written
by shell heredocs (`memory_reach_eval`, `memory_quality_eval`, `memory_native_eval`, `memory_pipeline_soak`,
`chat_hang_probe`, `import_probe`). The observer yardstick the register requires (`TECHNICAL-DEBT.md:536`)
reads the untracked `build/manuscripts/*` and its expected counts (717 / 348 / 948) live only in journal prose.
No target runs the eight `tools/test_*_mutations.py`.

### F10 — duplicated text rules with a concrete divergence (P2, debt; one defect)

Five sentence splitters (`answer_pipeline.c:896` `rel_next_sentence`, `:1253` `sentences`,
`observer_text.c:293` `obs_first_sentence`, `:375` `obs_sentence_end`, `observer_fake.c:88`), eight title lists
in `src/llm` plus Python's, two kinship lists (`answer_pipeline.c:832`, `observer_text.c:403`), three
number-word lists, two identical word-character functions in one file (`answer_pipeline.c:845,1227`).
**Defect:** the relation check's splitter knows four titles and no initials, so it cuts "Ms. Bennet" and
"James K. Powell" mid-name where the observer's splitters do not; a relation statement can be cut in two.

### F11 — the chat-title archive does not survive the file store (P3, defect)

`sdb_chat_rename` keeps the old title in `conversation_titles` (`store_chats.c:307-321`, migration 018, "delete
nothing, only archive"). The file export writes only the current title (`store_import.c:1038`) and the importer
reads only the last title line (`:344,354`) although the file store keeps every one (`chat_store.c:250-255`).
No product code reads the table. A file-to-database-to-file round trip drops exactly what the migration was
added to keep, and the column checklist's importer and file-store items were missed. (Migration 017's `is_note`
meets the whole checklist.)

### F12 — smaller debt (P3)

- `records_new_id` (`records.c:45-48`) adds a process-global (mutex, timestamp, counter) absent from the
  register's intentional list; `records_new_id_unused` (`:65-73`) returns a used id unreported once all 65,536
  attempts collide (defect, theoretical).
- `observer_text.c` now reads the flag table inside `tokens()`, so a text function depends on a process-global
  (24 `setenv`/`experiments_reload` calls in tests). `MAX_SENTENCE_PAIRS 96` (`observer.c:50`) has no measured
  source and sizes the conflict array on every path.
- `main()` `setenv`s `SCRIBODO_REMOTE_CONFIG` and `SCRIBODO_PROFILE` after worker threads start
  (`main.c:3935,3977`); `getenv`/`setenv` across threads is unsynchronised (latent). `SCRIBODO_FAKE_EXTRACT_MS` is
  read on every fake extraction and documented nowhere.
- Drafts are hashed and written with `fsync` on the UI thread every 10 s while dirty (`main.c:1680-1701`);
  unmeasured at whole-manuscript scale (GAP-003).
- The script host grew past its 2026-11-12 acceptance (`run_expect` 198 to 209, `script.c` 1,006 to 1,026) with
  no size limit on the tradeoff.
- `observer.c` `worker` 161 to 175 lines; `answer_pipeline.c` 1,105 to 1,626 lines.
- `tests/fixtures/observer_faults/{mars,pp}.json` say "never edited to match a result"; five anchors were
  corrected on 2026-11-15 (journaled, justified, not recorded in the file).
- `docs/REDTEAM-016-real-manuscript.md:70` carries a home-directory path (since `af57a32`).
- `tools/answer_*.py` 37 files (was 35), no shared `tools/lib`; `tools/memorilla_map_probe.py`'s experiment is
  closed; `build/` is 16 GB (9.8 GB the Memorilla downloads); 3,982 loose git objects, no packs.

### F13 — the pinned MariaDB image's tag moved upstream (P3, recorded)

`make dependency-report` fails: the tag `mariadb:11.4.13` now resolves to a different digest than the one
`docker/compose.yml:9` pins. The pin is by digest, so a Docker install still pulls the image that was verified;
nothing in the product changed. The report fails by design on a moved tag. Since Docker is now an install
option and native MariaDB is checked only for the 11.4 series (F6), the two paths can run different server
builds, and the report covers only the Docker one. Re-pinning means reviewing the new image, not editing a hash.

## Register claims re-checked

| Entry | Verdict at `3731db8` |
|---|---|
| Experiment flags lifecycle (P1) | Code rule holds (no experiment `getenv` outside `experiments.c`); record does not (F2, F4, F8) |
| Receipt and trace contract | Holds (done): fixtures, both-language test, relation line followed the procedure |
| Worker monoliths | Holds for the split stages (largest observer stage 81, chat builder 76); `worker` worse (175) |
| Findings logs with collided ids | Holds: cause fixed, no scan tool |
| Unarchived evidence | Environments done; evidence **recurred** (F1) |
| Duplication rows | Atomic write done; unit size, headers (`store.h` 38 includers), dead exports, model requests hold; tools and Makefile worse (49 rules, was 44) |
| Loopback volume in the vector sweep | Holds, unchanged |
| Spawned tools inherit descriptors | Partly: the supervisor and `server.c` use `POSIX_SPAWN_CLOEXEC_DEFAULT`, `services.sh detach` closes fds; `backup.c:107` does not; `server.c:330`'s `popen` is unlisted; the entry still names the supervisor as open |
| Storage policy leaks (P1) | Holds as amended (`memory_job.c:427,451`, `memory_documents.c:90,206`) |
| `main.c` bottleneck (P1) | Holds and drifting (F3) |
| Hidden process-global state (P1) | The index-text table is retired; `records.c` and the flag table are new and unlisted (F12) |
| CI omits high-value verification (P1) | Coverage improved (native services, `servicestest`, `crashtest` in acceptance); the text is stale (F8); `make tools` uncovered (F9) |

## Checked and in good shape

- Hygiene: `git fsck` reports only dangling blobs; 0 tracked-but-ignored files; all seven submodules at their
  pins; 0 `TODO`/`FIXME`/`HACK` in `src/`; 0 `#if 0`; every `tests/test_*.{c,py}` is wired into `make test`.
- Migrations 016 to 018 re-run cleanly (`IF [NOT] EXISTS`, `INSERT IGNORE` receipts) and every one of the 16
  named crash points in `src/` is driven by `test_crash_points.c`.
- The native services hardening is real: one mutating command at a time (`lockf`), pid files checked against this
  store's arguments, installs renamed into place, secrets checked non-empty, the purge guard; `servicestest`
  pins every REDTEAM-023 finding and runs in acceptance.
- Fail-closed reads (`records_list_files_checked`), the idempotent interrupted-import finish, chat appends that
  fail with their attachment.
- The receipt contract and the relation-check fixture are the model F2 and F7 should copy.
- `experiments.c` is the only reader of experiment flags, under `pthread_once`; gated observer code is inert
  when off (one branch; no judge-log writes without a log); the pair overflow is counted, not silently dropped.
- Server memory constants each cite a dated measurement; every new observer constant but those in F12 cites its
  journal step.

## Recommended order

1. **F1**: the 106 cited directories are archived (below). Still to do: move nothing new into gate-wiped paths,
   and add a check (`tools/evidence_check.py`: every cited `build/` path is archived or listed as non-evidence)
   to `make test`.
2. **F2**: give the step-8 arm its own setting again (the sentence flag keeps attributes; the subject co-signer
   gets its own level or flag) and add one shared fixture of canon/sentence/text cases with expected verdicts,
   read by both a C test and a Python test.
3. **F11** and the F10 defect: small product fixes with tests.
4. **F4, F5, F8**: pin arms in the measurement scripts and record them in each run; tie the UI figure to the arm;
   correct the standing docs and the register.
5. **F3, F6, F7, F9** as register entries with acceptance criteria; F12 as register rows.

## Register changes made by this audit

See the 2026-11-15 session log in [TECHNICAL-DEBT](TECHNICAL-DEBT.md).

## Evidence

The matrix logs, `results.tsv` and every cited `build/` path that existed in no archive are in
`scribodo-evidence-2026-11-15-audit004.tar.gz` (3,804 files; binaries, objects, the locked Python environments
and the sanitizer build excluded); see [EVIDENCE](EVIDENCE.md) for the manifest and SHA-256. After it, every
cited path that exists is archived except those exclusions. The four reviews' and the citation scan's scratch
scripts were session files and are not kept; every figure is re-derivable from the tree, the manifests and
`grep`, `wc` and `git log`.
