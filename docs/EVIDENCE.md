# Local evidence archive

Dated reports under `docs/` cite run directories and logs by their original
`build/` paths, for example `build/mn-z0h05o1h` in MEMORY-011 or
`build/memory-quality` in MEMORY-009. Those paths were working locations on the
development Mac, not tracked files. Nothing cited is deleted; it is archived.

| Archive | Contents | Where |
|---|---|---|
| `scribodo-evidence-2026-11-07.tar.gz` | Every `build/` directory and log cited by a doc as of 2026-11-07: the native continuity runs (`mn-*`, `mo-*`), the causal parity and bedrock probe runs (`mc-*`, `mb-*`), `memory-quality`, `memory-mutations`, `remediation-mutations`, `redteam2-fresh`, `hangs`, and the `memory-bedrock-*`, `memory-redteam-*`, `window-audit-*` and `async-history-retry-acceptance` logs | `~/Library/Application Support/Scribodo/evidence/` with `SHA256SUMS` and a `manifest-2026-11-07.txt` listing the top-level entries |

| `scribodo-evidence-2026-11-08.tar.gz` | The S2 and S3 reach runs (`build/memory-reach/runs/*`, including seeds 1 to 4), the four native model-side runs (`build/mn-*`: the sleep-interrupted run, the complete 144-answer run, the two near-miss runs), the stall replay (`build/stall-replay`), the hang probe (`build/hang-probe`) and the lexical bench reports (`build/lexical-bench`) | same folder, `SHA256SUMS` and `manifest-2026-11-08.txt` |

| `scribodo-evidence-2026-11-08b.tar.gz` | The canon-aware chat run (`build/mn-ld40aqkd`, MEMORY-020) and its episode fixture, the two document-reach runs rescored in MEMORY-017 and the two runs after the unit-cut change (`build/memory-reach/runs/scribodo_test_reach_{98605,98703,5807,5959}_*`), and `build/hangs` including the sleep-interrupted `test_llm` samples | same folder, `SHA256SUMS` and `manifest-2026-11-08b.txt` |

| `scribodo-evidence-2026-11-08c.tar.gz` | The real-manuscript work (MEMORY-021): `build/manuscripts/pride-and-prejudice` with the Gutenberg source and checksum, the imported chapters and parts, and under `runs/` every application run log, database sample, probe script and screenshot; the reach run over the novel (`build/memory-reach/runs/scribodo_test_reach_15740_*`) | same folder, `SHA256SUMS` and `manifest-2026-11-08c.txt` |

| `scribodo-evidence-2026-11-08d.tar.gz` | The red-team of the real-manuscript changes (REDTEAM-016): `build/manuscripts/pride-and-prejudice/runs/redteam016` (the explorer-scroll probe against the baseline and the fixed app, the baseline import test run) and the suite logs `build/make-{test,uitest,dbtest,tsan}-redteam016.log` | same folder, `SHA256SUMS` and `manifest-2026-11-08d.txt` |

The 2026-11-09 repository audit and follow-up red-team evidence is in
`scribodo-evidence-2026-11-09-audit.tar.gz` in the same folder, with
`manifest-2026-11-09-audit.txt` and an entry in `SHA256SUMS`. It contains 108 files:
the `build/audit-*.log`/`.json` and `build/redteam017-*.log`/`.json` run records,
the generated mutation sources/logs/results under `build/audit-mutations/`, and
the clean shallow checkout's Connector/C and analyzer logs. It excludes compiled
binaries and the shallow checkout itself. Archive SHA-256:
`796386b993c464e353a5a650c99f3c6f8e918d5b2d3a97e7622c075c810715f3`.

The hosted-CI iteration is preserved separately in
`scribodo-evidence-2026-11-09-audit-ci.tar.gz` (87 files), with
`manifest-2026-11-09-audit-ci.txt` and its `SHA256SUMS` entry. It includes the
initial hosted analyzer failure, trusted-acceptance result, corrected-tree
16-target rerun, refreshed mutation controls and fresh Connector/C build logs.
It precedes the corrective push's hosted rerun. Archive SHA-256:
`f4cdef24571a58d0d0ff1a1b71ef7ffe582e57c0345e2c372be6461e976828d5`.

The AUDIT-002 evidence is in `scribodo-evidence-2026-11-09-audit002.tar.gz` in
the same folder, with `manifest-2026-11-09-audit002.txt` and an entry in
`SHA256SUMS`. It contains 80 files: the baseline and remediated 17-target matrix
logs and results (`build/audit002-*.log`, `build/audit002-after-*.log`, both
`results.json` files), the clean-rebuild log, the hosted-CI attempt records for
`278259c`, the 40-run baseline log, the `TIME_WAIT` snapshot, and under
`build/audit002-segment/` the generated test variants, the instrumented client
copy and every run log including `before-fix.log` and `after-fix.log`. Compiled
binaries and the `build/audit002-clean` build tree are excluded. Archive SHA-256:
`fb20ec39da5ef058fd8545e6cfb6fa46d5ad1b23803d5de112d1c4f260cc89e3`.

The answer-cycle and REDTEAM-019 evidence is in `scribodo-evidence-2026-11-09-cycle.tar.gz`
in the same folder, with `manifest-2026-11-09-cycle.txt` and an entry in
`SHA256SUMS`. It contains 154 files: the income isolation run (`build/ai1`), the
strict-CPU arm (`build/pq2`), the boundary live capture and review
(`build/answer-boundary-capture`), the boundary run's exported messages, units,
work copy and logs (`build/product-memory-use/boundary-1`, frozen binary
excluded), the fake-backend dry run, the quotation-binding and fixture-binding
outputs, the REDTEAM-019 17-target matrix logs and results, and the generated
mutation sources and logs under `build/redteam019-mutations/` (binaries excluded).
Archive SHA-256: `b3e1f0b5ab008d5ae580b8f1c2d979dbecfdf73f6b852d995f966fffaeb5f575`.

To consult a cited path, extract the archive into an empty folder and read the
path relative to it. Stale `llama.sock` files inside a few run directories were
not archivable and are not evidence.

Rules:

- A doc keeps citing the original `build/` path; this file maps it to the archive.
- New evidence stays in `build/` until the next archive pass, which adds a new
  dated tarball here rather than rewriting an existing one.
- The archive folder is outside the repository and is never committed. It is
  covered by ordinary machine backups, not by project backups.

The answer cycle 3 and AUDIT-003 evidence is in
`scribodo-evidence-2026-11-11-audit003.tar.gz` in the same folder, with
`manifest-2026-11-11-audit003.txt` and an entry in `SHA256SUMS`. It contains
774 files: the offline coreference arms (`build/cr1`, `build/cr2`), the seven
pass runs of the verified pipeline (`build/answer-live-pipeline` to
`-pipeline7`), the three coreference treatment runs and their control
(`build/answer-live-coref`, `-coref2`, `-coref3`), the pipeline review outputs
(`build/answer-pipe-capture`), the gate replay sheets (`build/gate-replay-*.json`),
the REDTEAM-021 mutation sources and logs (`build/redteam021-mutations`), and
the AUDIT-003 matrix logs, results and the remediated `dbtest` log
(`build/audit003`). Instrumented binaries and objects are excluded (31 files).
Archive SHA-256:
`25668b343d4e8aac3ca6c30a49dcd6b46a1c6a7994e4de62dc0b8883acb8333e`.

The second manuscript's evidence (answer cycle 4, `journal/answer-cycle4_mars.md`)
is in `scribodo-evidence-2026-11-11-mars.tar.gz` in the same folder, with
`manifest-2026-11-11-mars.txt` and an entry in `SHA256SUMS`. It contains 630
files: the imported novel with its Gutenberg source and checksum
(`build/manuscripts/a-princess-of-mars`), the two reach runs at the same tree
(`build/memory-reach/runs/scribodo_test_reach_63447_*` for the second novel and
`..._63867_*` for the first novel's control), the two live runs' exported
messages, units, manifests, logs and work copies (`build/product-memory-use/mars-pipe0`
and `mars-pipe1`), the live-capture requests, streams and outcomes, the gate
replays before and after the capitalisation change, the gate logs and the
review sheets (`build/answer-pipe-capture/review-pipeline-mars{0,1}.json`), and
the instrumented builds' generated sources and build logs. Frozen binaries and
objects are excluded (6 files). Archive SHA-256:
`d2a305e62be15201e316c6c1d32e866a2d34d1f7c42277d49e9569e2b2ccbed8`.

The composer-window work (answer cycle 4, item one, `journal/answer-cycle4_window.md`)
is in `scribodo-evidence-2026-11-11-window.tar.gz` in the same folder, with
`manifest-2026-11-11-window.txt` and an entry in `SHA256SUMS`. It contains 505
files: the offline window replay (`build/window-replay/wr1`: plan, manifest,
every arm's request and response, results, server log), the live runs with the
rule inert (`build/product-memory-use/pipe-window1`, `mars-pipe2`), on the
intermediate build (`mars-pipe3`) and on the final build (`pipe-window2`,
`mars-pipe4`) with their captures under `build/window`, the review sheets, the
gate logs, and the three instrumented builds' generated sources and build
logs. Frozen binaries and objects are excluded. Archive SHA-256:
`3ffebd3e3427f08ee6dc63574f7ff24bd0979998dc495858cc05077ef8c6dee4`.

The P0 steps of 2026-11-11 and -12 (journals `answer-cycle4_flags`, `_receipt`,
`_margin`, `_relation`, `_heights`, `gap004-cycle1` to `-cycle7`,
`debt-environments`) are in `scribodo-evidence-2026-11-12-p0.tar.gz` in the same
folder, with `manifest-scribodo-evidence-2026-11-12-p0.txt` and an entry in
`SHA256SUMS`. It contains 656 files: the three receipt-era live runs
(`build/product-memory-use/mars-pipe5` to `-pipe7`, exported messages, units,
manifests and logs), the captured requests and replies of the flags and receipt
runs with their comparison scripts and gate logs (`build/window`), the margin
arm's three runs (`build/margin1` to `-3` and logs), the interruption matrix's
runs and every gate log (`build/crash`), the relation arm's outputs and gate
logs (`build/relation`), the atomic-writer and environment gate logs
(`build/atomic`, `build/envs`), the third novel's import with its Gutenberg
source and checksum (`build/manuscripts/wuthering-heights`), and the
instrumented live-capture builds' manifests and logs (`build/answer-live-flags`,
`-flags2`, `-receipt`, `-relation`). Binaries, objects and the work copies'
databases are excluded. Archive SHA-256:
`dfcc8e9a8530c015b90946a5b38fd4445be35014fb8f98dcb10719ae5b03984d`.

The rest of 2026-11-12 (journals `answer-cycle4_builder`, `_exact_span`,
`_retire_index`, `_tolerance`, `_relation_split`, `_note_turns`, the third
novel's reach section of `_heights`, `observer-cycle1_replay`,
`observer-cycle1_split`, `debt-script_host`) is in
`scribodo-evidence-2026-11-12-loose-ends.tar.gz` in the same folder, with
`manifest-scribodo-evidence-2026-11-12-loose-ends.txt` and an entry in
`SHA256SUMS`. It contains 6,362 files: every live run under
`build/product-memory-use` that no earlier archive holds (41 runs: the
pipeline, coreference, relation, builder, exact-span and retire runs on the
three novels, with their exported messages, units, manifests, logs and work
copies), the live captures and comparison outputs of the relation, builder,
exact-span and retire steps (`build/relation`, `build/builder`, `build/exact`,
`build/retire`), the tolerance and relation-split measurements
(`build/tolerance`), the observer replay baselines and split comparisons with
the seeded project copies and every gate log (`build/observer`), the third
novel's reach run and the first novel's control
(`build/memory-reach/runs/scribodo_test_reach_84012_*`, `..._91611_*`,
`build/reach-heights`), the review sheets and gate replay sheets
(`build/answer-pipe-capture`, `build/gate-replay-*.json`), and the
instrumented live-capture builds' generated sources, manifests and logs
(`build/answer-live-builder`, `-final`, `-exact`, `-retire`, `-ca908bd`).
Binaries, objects, dependency files and the frozen app copies are excluded.
Archive SHA-256:
`4016a1ff5e67639a24f3db62665d356653295b71f96899ed388870365a4f0465`.

Everything cited from 2026-11-12 to 2026-11-15 that no earlier archive held is in
`scribodo-evidence-2026-11-15-audit004.tar.gz` in the same folder, with
`manifest-scribodo-evidence-2026-11-15-audit004.txt` and an entry in
`SHA256SUMS`. It contains 3,804 files (21 MB compressed) under 66 top-level `build/` directories, most of them
the answer-cycle and red-team runs (`build/redteam020-mutations`, `build/ux1`..`ux4`, `build/answer-unity-query`,
`build/pcl1`, `build/answer-ri-capture`), 35 files of `build/gap003` and the AUDIT-004 matrix logs
(`build/audit004`).

**Correction (2026-11-15, AUDIT-004 remediation):** this paragraph first said the archive also held the observer
cycle's runs (`build/observer/*`), the GAP-003 series, the native-services move and REDTEAM-023's durability runs.
It holds no `build/observer` file. The audit's scan counted a cited path as archived when an older manifest
listed files under it, so directories that had grown since were skipped. `tools/evidence_check.py`, which reads
each tarball's real file list, found 75 more existing paths; they are in the `audit004b` archive below.

Excluded: binaries, objects, the two locked Python environments (`make envs`
recreates them) and the sanitizer build. Thirty-five cited paths existed
nowhere when the audit ran; AUDIT-004 F1 lists the ones that were evidence.
Archive SHA-256:
`2fa9b638d6422fcbc833b38f7dc70edf3bd9700640cef054235f611d5808277b`.

Everything cited that existed and that no archive held after the correction above is in
`scribodo-evidence-2026-11-15-audit004b.tar.gz` in the same folder, with
`manifest-scribodo-evidence-2026-11-15-audit004b.txt` and an entry in `SHA256SUMS`. It contains 6,130 files
(40 MB compressed), the list `tools/evidence_check.py --unarchived` printed:
- `build/observer`: the observer cycle's real-model runs, judge and gate logs, REDTEAM-023's durability and
  batch runs, and the native-services gate logs;
- the GAP-003, recovery, tolerance and relation gate logs;
- the memory-reach runs not yet archived;
- `build/product-memory-use/defaultpath-pp`;
- the REDTEAM-020/021 and MEMORY-022 logs.

Binaries, objects, debug symbols and sockets are excluded. Archive SHA-256:
`cdd4079ee949c70ed77a5f7a648e75289710590c368efbf93a4a2256d10362c5`.

The tooling remediation stream's gate logs (`journal/audit004-tooling.md`) are in
`scribodo-evidence-2026-11-15-audit004-tooling.tar.gz` (15 files), with its manifest and `SHA256SUMS` entry.
Archive SHA-256: `8333d169392724b61a11f2c55ac6fe8d39ce1ad1fe40f6d4f4630916f470e5b2`.

The 33 files the remediation's red team found under cited directories and in no archive (32 under
`build/gap003`: the gates-exit files, the hosted CI and acceptance logs, `restart-fake-3`, `restart-real-2`,
`series-real-3`, the soak and restart logs, `sysmem`; and `build/hangs/crash-points-183626.txt`) are in
`scribodo-evidence-2026-11-15-audit004c.tar.gz`, with its manifest and `SHA256SUMS` entry. Archive SHA-256:
`b3c358881bd7f3dd905348dfbb4d70fff7cc3cff4c977ff2e3fb61dbaf463939`.

The tooling stream's red-team and verification gate logs are in
`scribodo-evidence-2026-11-15-audit004-tooling-r.tar.gz` (12 files; SHA-256
`da7913a063ca00fbfaceb699142bc6f8c24060eee7a6d797eaaa497bdcca6d78`) and
`scribodo-evidence-2026-11-15-audit004-tooling-v.tar.gz` (12 files; SHA-256
`2141f13738b812f0800210cc175f045df363e10fa8c6096fd885c5e6645f3fe3`), each with its manifest and `SHA256SUMS` entry.

The remediation streams' gate evidence is in `scribodo-evidence-2026-11-15-audit004d.tar.gz` (1,195 files, 9.7 MB
compressed), with its manifest and `SHA256SUMS` entry:
- the observer stream's per-commit gate logs, exit files, gate scripts and yardstick runs
  (`build/wt-observer/build/audit004-gates`, including `yard-runs/`);
- the data/text stream's gate logs, exit files and red-team probes through its second round
  (`build/audit004-text/backfill`, `build/audit004-text/r1b`, `build/audit004-text/r2b`,
  `build/audit004-text/redteam` and the per-item logs beside them);
- the merge gates of `705415e` and `d419986` (`build/merge-gates/main-705415e`, `build/merge-gates/main-d419986`);
- the gate outputs left in the other worktrees (`build/wt-main/build/uitest-coord.log` and `build/wt-main/build/ui`,
  `build/wt-services/build/servicestest-logs`, `build/wt-text/build/crash/matrix.tsv`).

Binaries, objects and debug symbols are excluded. The journals keep citing the worktree paths. The archive holds
them under those same paths, so the citations stay checkable after the worktrees are removed. Archive SHA-256:
`df1c9555ab506d91f943ebf1788c47c41433472bb248bea45e2ceea516957d1e`.

The data/text stream's third verification round and final items are in
`scribodo-evidence-2026-11-15-audit004e.tar.gz` (114 files, 840 KB compressed), with
`manifest-scribodo-evidence-2026-11-15-audit004e.txt` and an entry in `SHA256SUMS`:
- the round-3 gate logs and exit files (`build/audit004-text/r3-2`, `build/audit004-text/r3-3`);
- the round-3 relation re-measurement (`build/audit004-text/r3-measure`);
- the F10 wave-2 observer yardstick runs, before and after the change, flag off and every arm
  (`build/audit004-text/r4-a`);
- the final-tree gate logs (`build/audit004-text/r4-gates`);
- the nine observer arm references the journal compares against
  (`build/wt-observer/build/observer/base-{sent,gate,both}-{pp,mars,heights}.txt`).

Archive SHA-256: `c96a25572e29be6a8bc3033b1b5ede68774d663514cd8735829a3c68c729d70d`.

The final AUDIT-004 verification matrix on `ac73f79` and the merge gates of `1f0660c` are in
`scribodo-evidence-2026-11-15-audit004-final.tar.gz` (29 files: `build/audit004-final`, including `results.tsv` and
every target's log, and `build/merge-gates/main-1f0660c`), with its manifest and `SHA256SUMS` entry. Archive SHA-256:
`cc016b9d88f76e605e9a4c5ab9e5db5aa2c8801c27bd089b6ed353a92df298c2`.

The data/text stream's final verification rounds, each with its manifest and `SHA256SUMS` entry:
- `scribodo-evidence-2026-11-15-audit004f.tar.gz` (3 files): the item-1 re-measurement (`build/audit004-text/f1`).
  SHA-256 `b08f4f1b1e15a5a147b1efd1895686d6b6912dd91aa12196909ea7124824f222`.
- `scribodo-evidence-2026-11-15-audit004g.tar.gz` (14 files): its gate logs and exit files (`build/audit004-text/f-gates`).
  SHA-256 `72f22c30b25b58fee9ff1048f7d71ae422f2f52493cb0d1844eb028082a133b5`.
- `scribodo-evidence-2026-11-15-audit004h.tar.gz` (2 files): the second pass's re-measurement (`build/audit004-text/g1`).
  SHA-256 `3eb8c9aa54f4a55013eba4eaa788e61e2767fb14fae44e4b682ac0d0644f4297`.
- `scribodo-evidence-2026-11-15-audit004i.tar.gz` (10 files): the second pass's gates on `45b001f`
  (`build/audit004-text/g-gates`). SHA-256 `98134062d3bf77cf469804706267f44aee8729dd7b4bdd719bb9ac012bbd1c5b`.

The services stream's Docker-mode work of 2026-11-16 (journal `audit004-services.md`, "With Docker running") is in
`scribodo-evidence-2026-11-16-docker.tar.gz` (17 files, `build/audit004-services-docker`), with
`manifest-scribodo-evidence-2026-11-16-docker.txt` and an entry in `SHA256SUMS`:
- the step-10 rehearsal with main's `services.sh` (the migration's restore leaving the native store down) and with the
  fix (`A-before-fix-*`, `A-after-fix-*`);
- the stale `scribodo-st-h` volumes removed by exact name (`C-stale-volumes.log`);
- the planted interrupted-run state, the two back-to-back Docker rehearsals and the Docker state after each
  (`B-*`);
- `backuptest` against a throwaway Docker store, from `up` to purge (`backuptest-*`).

Archive SHA-256: `7cc03f8c0354d4ba822138cfe51ac30b2b22a2ae10b0340956b7f46ed8c15212`.

The red-team round on that work (journal `audit004-services.md`, "Red team of remed/services-docker") is in
`scribodo-evidence-2026-11-16-docker-r.tar.gz` (12 files, `build/audit004-services-docker-r`), with
`manifest-scribodo-evidence-2026-11-16-docker-r.txt` and an entry in `SHA256SUMS`:
- the Docker rehearsal with the pre-red-team `services.sh` and the new cases, twice (`before-fix-*`,
  `before-fix2-*`: the empty-store, mid-load-kill and partial-restart cases failing);
- the two back-to-back rehearsals with the fixes and the rehearsal leftovers after each (`after-fix-run{1,2}.*`).

Archive SHA-256: `39dc390101d8e0830ad656c165e74615868f945c415ce45bcaf448c13949f028`.

The final verification items on that work (journal `audit004-services.md`, "Final verification items") are in
`scribodo-evidence-2026-11-16-docker-f.tar.gz` (20 files, `build/audit004-services-docker-f`), with
`manifest-scribodo-evidence-2026-11-16-docker-f.txt` and an entry in `SHA256SUMS`:
- the Docker rehearsal with the previous `services.sh` and the new tests (`before-fix-*`);
- the two intermediate pairs in which the unwritable-output case still failed, the progress line leaking into the
  MariaDB version check (`intermediate-run{1,2}.*`, `intermediate2-run{1,2}.*`);
- the two back-to-back rehearsals with the final script and the leftovers after each (`after-fix-run{1,2}.*`).

Archive SHA-256: `0ef506e2d2e19ae5b0643987ad7ffbafc1e409274d1c9594c6d2f4bdec805de9`.

Since 2026-11-15 `make test` runs `tools/evidence_check.py`. Every cited `build/` path must be one of:
- archived: checked against each tarball's real file list, so a cited directory counts only when every file under
  it is archived, and a file stem (`build/recover/gates2`) resolves to its files;
- declared not evidence (`tests/fixtures/evidence_not_archived.txt`), by exact entry, with `DIR/**` for a subtree;
- recorded lost (`tests/fixtures/evidence_lost.txt`). A lost path that reappears on disk fails.

A missing archive folder fails, except on a GitHub-hosted runner (`CI` and `RUNNER_ENVIRONMENT=github-hosted`) or
with `SCRIBODO_EVIDENCE_SKIP` set.

The room-per-token measurements (journal `room-per-token.md`, MODEL-001 "Room per token") are in
`scribodo-evidence-2026-11-17-room.tar.gz` (21 files, `build/room`), with
`manifest-scribodo-evidence-2026-11-17-room.txt` and an entry in `SHA256SUMS`:
- each run's server log at `-lv 4` and its JSON record (`A0`, `A5`, `D0`, `D5`, `M0`, `M1`, `M2`);
- the precision runs (`P0`, `P0b`, `P5`) and `summary.txt` with the comparison.

Archive SHA-256: `7b03232848a0c2762e9ac455b4478abbcb875182e584865462f272edbb473f48`.

The red-team follow-up on that work (journal `room-per-token.md`: "Fixed cost, as macOS sees it", "Long prompts", the
in-app chat, "Launch") is in `scribodo-evidence-2026-11-17-room-b.tar.gz` (79 files under `build/room`), with
`manifest-scribodo-evidence-2026-11-17-room-b.txt` and an entry in `SHA256SUMS`:
- the fixed-cost runs `F5s`, `F5sb`, `F5`, `F5b`, `F5c`, `F0`;
- the long-prompt runs `L0`, `L5` and `summary2.txt`, `summary3.txt`;
- under `app/` the in-app chat's script, its server lines and the two launch logs;
- under `scratch/` the earlier single-change runs with the harness they came from.

Archive SHA-256: `6e5466ca5229e20794607afec072998c3a3a6a7d0dfa6797e381b41ac78330b7`.

Its last verification items (journal `room-per-token.md`: the full-pool runs, the morning launch) are in
`scribodo-evidence-2026-11-17-room-c.tar.gz` (6 files under `build/room`), with
`manifest-scribodo-evidence-2026-11-17-room-c.txt` and an entry in `SHA256SUMS`: `G5`, `G5b`, `summary4.txt` and
`app/launch-morning.log`. Archive SHA-256: `c5f50a2bf0b0bc6d0535dce5015a169d37c1372ab379825239f9b01fcc547003`.

The first COMPOSITION-001 C03 compatibility run is in
`scribodo-evidence-2026-11-26-composition001-c03-parity01.tar.gz` (**201 files, 14,370,535 bytes**),
with a JSON per-file hash manifest and a same-basename `.sha256` sidecar in the existing evidence folder:

- `c03/parity-01/`: full F32 reference distributions, baseline/candidate replies and verbose server logs,
  artifact identity metadata and comparison;
- `c03/candidate-build/`: CMake cache and generated link commands;
- `c03/reference-source/`: pinned reference Python/configuration files, not weights;
- `source/`: the capture/probe/review scripts, their logs/public-source evidence and unchanged legacy helpers.

All archive members were read back and SHA-256 checked without extraction. No model weights, binaries,
author data or API-key files are included. The malformed-input proposal was excluded and remains unexecuted.
Archive SHA-256: `201847a31561b2c9383ad059eb52e268df19e1e330e2e949c9e2c069bd61ffc1`.
The receipt is `journal/composition001/c03/parity-archive.json`. These observations are first-pass
compatibility evidence, not production pin adoption or final acceptance.

The REDTEAM-028 working-tree verification is in
`scribodo-evidence-2026-11-27-composition001-c03-redteam01.tar.gz`
(**264 files, 14,765,482 bytes**), with per-file JSON manifest and `.sha256` sidecar.
`c03/redteam-evidence01/` contains fresh `parity-02` distributions and all `redteam-gates01` live adapter,
schema/slot/Qwen-role, full test/analyzer/sanitizer/TSan logs, raw bodies and exit ledgers.
It also snapshots C03 tooling, the Makefile/regression suite, unchanged helpers and candidate CMake/link
configuration. All member bytes were rehashed on read-back; no weights, binaries or API-key files.
SHA-256: `305cbbdef4f240ab4db8264905548dcea0e9772654ce7432972033b5e3d3149a`.
Receipt: `journal/composition001/c03/redteam-archive.json`. Subsequent receipt/doc/push-driver edits
are not retroactively part of that immutable archive.

The conditional committed-snapshot verifier writes a separate
`scribodo-evidence-2026-11-27-composition001-c03-committed01.tar.gz`, per-file manifest and checksum,
before permitting the push. Its exact receipt is written to the runtime scratch directory as
`committed01-archive-receipt.json`; the pipeline must exit 0, not merely create a tarball.
