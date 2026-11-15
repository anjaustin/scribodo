# REDTEAM-023: adversarial review of the native services and the micro-batch

**Date:** 2026-11-14 (evening)
**Reviewed tree:** `a0101f2` (the native services move `d77f35e`, the default-path measurement `e1482c5`) with
the uncommitted micro-batch change (the chat server's `-b`/`-ub` from 256 to 128, the fit's per-token figure).
**Direction:** the author: "Red-team the changes, remediate your findings, verify remediations, and iterate until
pristine."

## Method

Two read-only reviewers, one on the native services (the scripts, the CI job, the app's supervisor, the docs), one on
the micro-batch, the no-candidates fallback and every number and causal claim written that day. Each finding was
traced before it was fixed; what could be measured was measured on this Mac rather than argued. The native mode's
findings are pinned by a new rehearsal, `make servicestest`, that runs on throwaway stores with their own ports and
never touches the author's.

Before the reviews, two failures from the micro-batch change's own gates were explained first: the memory-fit test
pinned the old measurement (a real finding, R1), and the real-model observer script had stopped with "app exited":
the Mac restarted at 17:20, MariaDB logged a normal shutdown, and two full reruns of the real-model suite passed 35
of 35.

## Findings and remediations

### The native services

**R1 — P1: `migrate-from-docker` replaced whatever the native store held, with no copy (fixed).** It loaded the
containers' dump over the native `scribodo` database unconditionally. It now refuses when the native store holds any
table unless `SCRIBODO_MIGRATE_REPLACE=1`, which dumps the native store beside the secrets first. Rehearsed:
a second migration over native data exits 2 and changes nothing.

**R2 — P1: an existing Docker user upgrading got an empty native store (fixed).** With no mode file (none was ever
written before native mode existed) and the native binaries installed, the mode was native, `up` initialised a fresh
data directory with the same credentials and port, and the app would have reported the author's project missing from
the store and offered to re-import stale legacy files under a new id. The mode now falls to Docker when credentials
exist without a native store, and native `up` refuses to initialise over such credentials unless
`SCRIBODO_NATIVE_FRESH=1`. Rehearsed (step 7).

**R3 — P1: a failed or concurrent migration (fixed).** The mode became native before the load, so a failed load left
the app on a half-loaded store; nothing stopped the migration while the app wrote. Now: refused while Scribodo runs,
the mode is written only after the load and its schema check, and any failure puts both stores back as they were
before the migration (see the second and third passes for how). Rehearsed: the refusal while the app runs (step 9), a second migration refused before anything is written (step 10), and a migration whose load genuinely fails (a view whose definer the sandboxed restore user cannot impersonate), after which the mode, the containers and their data are as before and the native directory it created is gone (step 10).

**R4 — P1: the daemons inherited the app's descriptors and its process group (fixed).** The app spawned
`services.sh` without `POSIX_SPAWN_CLOEXEC_DEFAULT`, the script started the daemons with `nohup … &`, so mariadbd
and Qdrant could keep the app's model and store sockets open for their lifetime, and a cancelled `up` signalled the
whole group, daemons included. The supervisor now passes only stdin, stdout and stderr (to `/dev/null`); the script
starts each daemon through a small detacher that calls `setsid`, sets the working directory beside its data, and
closes every descriptor above 2. Rehearsed (step 5: a descriptor open in the caller is absent from both daemons,
and each is its own process-group leader). The same detacher fixed Qdrant writing `.qdrant-initialized` into the
caller's directory (one had appeared at the repository root).

**R5 — P2: an interrupted first start left root without a password for good (fixed).** First-time setup was decided
by the data directory's existence, so a setup interrupted after `mariadb-install-db` never ran again. It now runs on a
server started with `--skip-networking` (the private socket only, so no account is reachable over TCP before it has a
password), tries root without and then with the generated password, and writes a marker only on success. Rehearsed
(step 3, both an install with no setup and a marker removed after a completed one).

**R6 — P2: accounts the container never had (fixed).** `mariadb-install-db` creates anonymous accounts, root at the
machine's host name with no password, a `test` database and PUBLIC grants on `test%`. Every `up` now removes them,
and prunes the grants of dropped test databases (about 130 had accumulated on the author's server in one day; a
test database's grant now goes when it is dropped). Verified on the author's live server and rehearsed (step 1).

**R7 — P2: the fallback socket path (fixed).** A long secrets path put the socket at a predictable path in `/tmp`.
It now lives in a directory under `/tmp` named for the user and folder, created `0700`, and refused unless it is a
real directory this user owns with that mode.

**R8 — P2: two `up`s at once (fixed).** Every mutating command (up, down, purge, migrate, rotate-root) runs under
`lockf` on a lock file beside the secrets. Rehearsed (step 2: two concurrent `up`s start one MariaDB and one Qdrant).

**R9 — P2: the purge guard (fixed).** It compared strings, so a trailing slash or a doubled one bypassed it, and it ran
after the services had been stopped. It now compares the folder by identity (`-ef`), runs before anything stops, and
covers the Docker volumes of the default compose project too; the acceptance job purges only a store under its own
temporary folder. Rehearsed (step 6, both spellings refused, the services still running, the confirmed purge done).

**R10 — P2: stale pid files (fixed).** A pid file was trusted by `kill -0`, so after a reboot a reused number could
stop Qdrant from starting and make `down` signal an unrelated program. A pid now counts only when its process is the
program it names. Rehearsed (step 4).

**R11 — P2: a daemon that died was never restarted (fixed).** The app started the services once per session. It now
starts them again when the store drops, at most once a minute; `up` leaves running daemons as they are.

**R12 — P2: a forced mode was remembered (fixed).** `SCRIBODO_SERVICES=docker up` wrote the mode file, so a one-off
look at the old containers switched the app back to them. A forced mode is no longer remembered.

**R13 — P3: Homebrew's MariaDB is not pinned (narrowed).** The script now refuses a MariaDB outside the 11.4 series;
patch releases still move with `brew upgrade`, which LANDSCAPE row 19 now says. `--no-defaults` keeps a `~/.my.cnf`
or Homebrew's `my.cnf` from changing what the server or any client does. CI's service logs go to the job's temporary
folder and are uploaded on failure; `make qdrant` deletes a binary that fails its version check.

**R14 — P2: the durability claim (corrected, R9 of REDTEAM-022 reopened as narrowed).** The README, LANDSCAPE row 9
and the journal said the native store's fsync "reaches the disk" so a commit survives power loss. Measured: native
commits take 0.24 ms, the cost of a plain `fsync` (0.05 ms on this disk), not of `F_FULLFSYNC` (4.0 ms; three runs kept in `build/observer/redteam023/durability/` with their scripts; a first, unkept run read 0.30, 0.04 and 3.6), so the
drive's cache is not flushed on commit. The text now says so; the crash matrix's slowdown natively (543 s against 78
s) is recorded without a cause.

### The micro-batch and the record

**R15 — P3: the fit figure's arithmetic (fixed).** The comment called 17 KiB "rounded up" from 12.2 + 4.3, leaving out
the CPU buffer. As allocated at 128 the total is 779 + 278 + 21 MiB, 16.8 KiB a token, which 17 covered; llama.cpp's
own projection, which counts the CPU buffer twice and which the reviewer and the first correction both read, is 17.2.
The figure is 18, covering both, and `test_components` pins it against the projection and the batch.

**R16 — P2: the replay that showed 32K used the wrong unit (corrected).** `SCRIBODO_MEM_AVAILABLE_MB` is MiB and the
replay passed a decimal-MB figure, 500 MB too generous; at that budget the old figure fit 32K too. Replayed at the
real launch's budget and both ends of its rounding (9,755 to 9,851 MiB): 32K every time with 18 KiB, against 5.08 GB
needed and 5.15 to 5.25 GB free, a margin of 70 to 170 MB, smaller than the swings between launches on the live
machine. The record says "usually 32K", and drops the unmeasured explanation it gave for two launches that fell to 8K.

**R17 — P2: "no change to the numbers the model computes" was asserted, and false.** Measured with a kept tool
(`tools/model_batch_compare.py`, `build/observer/redteam023/batch/`; the kept logs come from the tool before it gave its loopback server an API key, which changes nothing it measures): twenty captured composer requests replayed with
their own sampling settings and seed; two runs at 256 agreed on 20 of 20, 128 against 256 on 17 of 20, the three
diverging after 277 to 1,357 characters on hard questions where neither version is better. The BF16 contract holds;
byte-for-byte replay against records made at 256 does not, so the thirteen replay tools that launch the chat server
as the app does now use 128, and the record says what changed. Prefill at 128 measured within the drift between two
256 runs (the machine warmed between them); generation unchanged by medians of three.

**R18 — P2: the capacity branch could never fire (fixed).** When every reranker score fails, `scored` is 0 and the
status is `rerank_unavailable`; the branch tested `scored == score_failures` with `scored` non-zero. It now tests
the status, and the notice gives the refused count.

**R19 — P2: "no candidates" is not always "nothing to verify" (fixed).** With documents attached, no candidates means
chat units crowded the documents out or hydration failed; the fallback sent those turns to the unverified recall
path. It now applies only when no documents are attached; otherwise the verified shape stands.

**R20 — P3: the fallback's notice was overwritten (fixed), and nothing pinned a fallback (partly fixed).** The recall
stage replaced the notice; it now keeps the pipeline's reason in front. `make uitest-db` runs the memory-recall script
a second time with the verified pipeline requested. Its first run of this round failed, and the failure was a product
defect (R22 below): the fake backend has no chat server, so the pipeline stopped before searching, returned status
`none`, and the turn was rebuilt around "No verified passage". With R22 that run now pins the could-not-run fallback:
the chat is recalled and its sources shown. The no-documents fallback itself (R19) is verified only by the real-model
soak recorded in the journal; no automated test reaches it, because the fake backend cannot run the pipeline.

**R22 — P2: a pipeline that could not run was presented as a search (fixed).** `answer_pipeline_run` returns before
searching when there is no chat server, no question part, or an allocation fails, leaving status `none` and zero
candidates; the stage then rebuilt the request around "No verified passage was found", the claim REDTEAM-022 R10
removed for other cases. When the pipeline did not run and searched nothing, the turn now goes to the recall plan with
"Verified pipeline could not run" in front of its notice.

**R21 — P3: claims and artefacts (corrected).** "Cost the chat model three quarters of its context" (the VM was the
difference between 8K and 24K, not 32K); "1.9 GB brings 32K back with room to spare" (it brought 24K); "services that
use 0.3 GB" (the containers showed 0.8 GB by `docker stats`, which counts page cache); LANDSCAPE rows 10, 13, 17 and 19 brought up to date; artefacts cited under
`build/ui`, which `make uitest` deletes, are now kept under `build/observer/redteam023`; the micro-batch measurement is
a kept tool, `tools/model_batch_compare.py`, with its logs. The reviewer's suspicion that two crash-matrix logs were
a copy was checked: the second run left its own throwaway databases' grants in the author's server, and the test
prints nothing run-specific but its time, which matched to the second.

## Second pass over the remediations

A fresh reviewer read the remediations; no P1, seven P2, all fixed and rehearsed:

- **A failed migration could leave a native data directory that became the store by default** (the mode was decided
  by its existence). The migration writes the current mode down before it starts, and its restore removes a native
  directory it created.
- **`set -e` does not apply inside a function called on the left of `||`**, which is how the migration called
  `native_up`, so a failed setup could have carried on to a TCP server with an unprotected root. Every step of
  `native_up` and `native_init` now checks its own result.
- **Some migration failures exited without the rollback** (a failed replace dump, a failed `compose down`), and a
  failed load after a replace left the native store clobbered. All now roll back, and the rollback reloads the
  native store's previous contents from its dump.
- **The rollback and the refusal brought the containers back even when they had not been the live store**, and
  without restoring the secrets' permissions the containers need. One `restore` now puts each side back exactly as it
  was (containers up or down, native services up or down), with the containers' permissions, and a migration over an
  existing native store is refused before anything is touched.
- **A purged native store looked like an old Docker user's folder** (credentials, no native data). Purge now
  remembers the native mode, so the next `up` starts a fresh native store; the credentials stay for an older container
  volume. Rehearsed (step 6).
- **The app-running refusal matched any Scribodo on the machine**, which would have failed CI on the author's Mac
  whenever the app was open. It now applies to the author's own store only; the rehearsal tests it on a default store
  under a throwaway home (step 9).
- **The rollback claims were not rehearsed.** Step 10 now makes a migration's load genuinely fail and checks both
  stores afterwards.

Also taken from the plausible list: pid files must name this store's own data or configuration in their arguments,
not just the program; the install runs into a temporary directory renamed into place; the descriptor sweep is bounded
by the system's limit; the rehearsal's `down` step now really gives `down` a pid file naming another program, a
long-path store checks the socket fallback, every store's logs are kept for CI, and the first container start after
Docker launches is retried; `rootsql` and `dump` refuse an invalid mode instead of falling through to Docker; the
comparison tool's server takes an API key; the probe tool defaults to the app's batch; the second store-UI run keeps
its own screenshots; the reranker notice says zero refusals means not ready; the README says a stopped Qdrant is
restarted only with the database. The author's live store, set up before the marker existed, went through the
one-time setup while the app was closed, its contents unchanged (1 project, 24 messages, schema 18).

The pass also caught a defect of its own making in this round: a bulk rename turned the two new pid-check helpers
into functions that called themselves, and `up` hung; the rehearsal failed every step that started a service, and
the helpers were restored before anything was committed.

## Third pass

A third reviewer read the second pass; no P1, two P2, all fixed:

- **A part-written native dump could have been reloaded over the intact native store** during a replacing migration's
  rollback: the reload path was set before the dump finished. The dump is now written to a part file, checked
  complete, renamed, and only then used.
- **A forced purge remembered its mode,** so purging a stray native copy with the mode forced could switch a Docker
  user to an empty native store. The purge now goes through the rule that a forced mode is never remembered.

And from the plausible list: the restore checks each of its steps and says afterwards whether it succeeded rather
than before (it runs under `set -e`, and a failed `rm` would have stopped it before the containers came back); a
native reload first empties the database, so a table only the failed load created does not survive; the secrets
folder has one spelling for everything that names it (a trailing slash or `/tmp` against `/private/tmp` would have
made a running store look stopped to the pid markers); each generated secret is checked non-empty; a data directory
without system tables is replaced rather than swallowing the new one. The rehearsal's checks were tightened where
they could pass without proving their claim: step 5 requires `up` to succeed and both daemons to be alive; step 9
requires the stand-in app to be running; the failing migration must have failed at the load (the containers' dump
taken, the sandboxed user refused) and leave no Qdrant behind; and two cases never exercised now are: a pid file
naming another store's live Qdrant (this store's starts, the other is spared), and a replacing migration whose load
fails (the native store keeps exactly its own tables and contents, the containers stay down as they were). The
rehearsal's cleanup also removes its compose projects if a run is interrupted.

## Verification

On the final tree, against the author's native store (`build/observer/redteam023/`, start and end times in
`gates-summary.txt` and `gates2-summary.txt`, every exit status 0): `make test` 95,767 C checks and 193 Python tests;
`make analyze` 0 warnings across 87 files; `make sanitize` 95,767; `make tsan` 48,340; `make dbtest` 18,006; `make
crashtest` 133 rows, 1,478 checks, 0 failures (run on the tree before the last script changes, which it does not
exercise); `make uitest` 409; `make uitest-db` 304 (281 before, plus the memory-recall script's second run with the
pipeline requested); `make uitest-real` 35; `tools/backuptest.sh` 17 of 17; `make servicestest` 17 steps without
Docker and all 21 with it (`servicestest-docker.log`), no rehearsal container or volume left. The first full gate run
of this round failed in two places, both found and fixed before this one: the second store-UI run (R22) and two
rehearsal steps that searched for the unnormalised `/tmp` spelling after the secrets folder gained one spelling.
