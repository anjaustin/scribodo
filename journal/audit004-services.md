# AUDIT-004 services stream — 2026-11-15

Scope: the services layer's debt from AUDIT-004 F6 and F13 and the register entries "Open — the services layer"
and "Open — spawned tools inherit the app's descriptors". Worked in the `remed/services` worktree, one commit per
item. Gates are recorded with their exit status; logs were kept in the session's scratch folder while the work ran.
Never run against the author's store: every script run here used a throwaway `SCRIBODO_SECRETS`.

## Item 1 — the script's reasons reach the author

**Before:** `service_supervisor.c` sent `services.sh up`'s output to `/dev/null` and kept `WEXITSTATUS == 0`. Every
refusal was one "Project store offline".

**Contract, written once:** `src/app/service_supervisor.h` (`SERVICES_EXIT_*`) and the header comment of
`tools/services.sh`, which `tests/test_services.c` parses and compares with the enum (same eight codes, each once).

| code | meaning |
|---|---|
| 0 | done |
| 1 | any other failure |
| 2 | refused, the author decides (credentials whose store is still in the containers; purge/migration guards; usage) |
| 3 | MariaDB missing or not the pinned version |
| 4 | Qdrant not built |
| 5 | a service port taken by another program |
| 6 | docker mode, Docker not running |
| 75 | another `services.sh` held the store's lock for five minutes (lockf's EX_TEMPFAIL) |

Codes 3–6 are kept exclusive: the script says them through `leave N`, and an EXIT trap turns any other exit with
3–6 (a command failing under `set -e`, e.g. curl's 6 or 3) into 1. `native_need` checks Qdrant before MariaDB so
that with both missing the code (3) and the last line (MariaDB's) agree.

**App:** the supervisor gives the script a stderr pipe (stdout stays `/dev/null`; still `CLOEXEC_DEFAULT`, so the
daemons, which the script detaches with descriptors 3+ closed and 1-2 on their log, never hold it), keeps a 1 KiB
tail, and reports `exit_code` and the last non-empty line (`reason`, 240 bytes, control characters made spaces). A
descendant still holding the pipe is not waited for: the read stops when the script is reaped.
`service_supervisor_describe()` gives "<sentence for the code>: <script's line>"; main.c shows it once in the status
line when it changes, and the sentence alone on the offline badge ("● Project store offline · Docker isn't running").
The headline is cleared when a start succeeds or the store answers. main.c change: one field outside the unions,
eight lines in the poll block, three in the label.

**Test:** `tests/test_services.c` (in `make test` and `make sanitize`): fake scripts for every code plus an unknown
one (code, line, sentence, description), success, a silent failure, a megabyte of stderr (no block, bounded), a
5,000-byte line (bounded), control characters, a missing script (exit -1, "couldn't run …"); then the real
`tools/services.sh up` on throwaway stores with stand-in binaries, refusing before it starts anything: no MariaDB
(3), the wrong MariaDB (3), no Qdrant (4), neither (3, and the line is MariaDB's), credentials without a native
store (2, "migrate-from-docker"), and a port held by the test itself (5, the port in the line). A mutation (port
refusal returning 1) fails the test.

**Gates (item 1 tree):** `make test` exit 0; `make analyze` exit 0 (0 warnings, 87 files); `make sanitize` exit 0.

## Item 2 — the defaults written once

**Before:** ports 33061/6333 in `services_lib.sh`, `compose.yml`, `db.c`, `semantic.c` (and `tests/db_admin.c`); the
secrets folder in `services_lib.sh` (twice), `db.c`, `semantic.c`, `remote.c`, `backup.sh`, `tests/db_admin.c`; the
MariaDB server flags in `compose.yml` and `services.sh`.

**Now:** `tools/services.defaults` (plain shell assignments) is the source. `services_lib.sh` sources it and exports
`SCRIBODO_MARIADB_SERVER_FLAGS`; `services.sh` builds `mariadbd`'s arguments from it (output byte-identical to the
old list, checked by evaluating `mariadbd_args` before and after); `docker/compose.yml` keeps no copy: its host ports
are `${SCRIBODO_DB_PORT:?run tools/services.sh}` / `${SCRIBODO_QDRANT_PORT:?…}` and its MariaDB `command` is the
string `${SCRIBODO_MARIADB_SERVER_FLAGS:?…}`. Checked with `docker compose config` (the CLI only, no daemon):
the string becomes the same five-element command list, the ports resolve, and an unset port fails with "run
tools/services.sh" (as the unset secrets folder already did). The C side reads `src/app/service_defaults.h`
(`db.c`, `semantic.c`, `remote.c`, `tests/db_admin.c`); `backup.sh`'s redundant default after sourcing the library is
gone.

**Decision:** a tracked header plus a test, not a generated header. Generating it would need an order-only
dependency in every rule that compiles those sources directly (the fault, crash and tsan builds, `analyze`), in a
Makefile other streams are editing; the test makes the two copies one value in practice.

**Gates (item 2 tree):** `make test` exit 0; `make analyze` exit 0; `make sanitize` exit 0.

**Test:** `tests/test_service_defaults.py` (in `make test`): compiles the header and compares its values with the
file; sources the library under a throwaway HOME with no `SCRIBODO_*` set and compares ports, secrets folder and
exported flags; checks `mariadbd_args` uses the variable; checks Compose has no defaults; fails if any of the product
files writes the port, the folder or a server flag literally. A mutation (header port 33062) fails it.

Not changed: the in-container ports in Compose (3306, 6333, the healthcheck) are the images' own; the experiment and
benchmark tools under `tools/` that still default the port or folder themselves (`history_bench.sh`,
`memory_pipeline_soak.*`, `memory_quality_eval.*`, `memory_reach_eval.c`, `store_growth_sample.sh`, `backuptest.sh`,
`offlinetest.sh`) are outside the product path and this stream's scope.

## Item 3 — the Docker path: written once, and covered without Docker

**Root operations written once.** `rootsql`, `dump_db` and `load_dump` each had a native body and a Docker body
(`services_lib.sh:63-118`). They are now one POSIX sh script (`root_ops_script`) run by `root_op`: on this Mac
through `/bin/sh` in native mode, inside the MariaDB container through `compose exec -T mariadb sh -c` in docker
mode. A two-line prelude per mode supplies the root password file (the secrets folder / `/run/secrets`), the host a
one-time account connects from (`127.0.0.1` / `localhost`) and `client TOOL …` (natively `--no-defaults` first, the
TCP address and `--skip-ssl-verify-server-cert`; in the container the tool as is). Passwords still reach the clients
only through `MYSQL_PWD`. One behaviour change: native `load_dump` now stops if creating the one-time user fails
(it used to go on and load as a user that did not exist); the Docker body already did (`set -e`).

**Qdrant's key, one header.** Qdrant accepts both `api-key:` and `Authorization: Bearer`; the app sends Bearer
(`src/llm/http.c`), so does `qdrant_delete_collections`, and `backuptest.sh`; `services.sh`'s readiness check was the
one `api-key:`. Both scripts now go through `qdrant_curl` (Bearer, key from a descriptor). Evidence that Qdrant 1.20
accepts Bearer with the plain API key: the app's semantic index has always authenticated that way (dbtest's vector
suites), and `make servicestest` step 1 now waits for HTTP 200 through `qdrant_curl` (result below).

**migrate-from-docker extracted.** The ~115 inline lines are `migrate_from_docker`, `migration_restore` and
`migration_rollback` (the two nested functions moved out, renamed); the `case` entry is one line. Moved verbatim,
state kept global, called as a plain command so `set -e` holds inside as it did inline.

**Coverage without Docker:** `tests/test_services_docker.sh` (in `make test`). `docker` and `curl` are bash functions
exported to the scripts (a function wins over PATH, so the real `~/.docker/bin/docker` the library puts first is
never reached; exported functions survive `lockf`'s re-exec, checked first in isolation). The shim records every
call and, for `compose exec`, runs the in-container script here with `/run/secrets` and the MariaDB clients faked,
so the container half is executed, not just named. The same three operations run in native mode against the same
fake clients and are held to the same transcript (only the connection arguments and the account host differ). 21
checks: rootsql/dump_db/load_dump in both modes (arguments, stdin, root password by environment, one compose exec,
the one-time user's create/sandboxed load/drop with the same random password, a failed load's status kept and the
user still dropped); `qdrant_delete_collections` (Bearer, only the database's collections); no `api-key` left;
`services.sh` in docker mode: `up` (info, compose up --wait, ps; secret modes; forced mode not remembered), `up`
without Docker (exit 6), down/status/logs, purge refused for the author's project and done for a throwaway one,
`testdb create`; `migrate-from-docker` refusing a forced mode (2) and without Docker (6, nothing written).
Before the refactor the same test passed every Docker check except two of my own exact-text expectations (trailing
spaces, then corrected) and failed the native ones only on argument order: the transcripts are equivalent.

**Gates (item 3 tree):** `make test` exit 0; `make analyze` exit 0; `make sanitize` exit 0.

Not covered: a whole migration (it needs a real native MariaDB and real containers); that stays servicestest step
10, which still needs Docker.

## Item 4 — finding the scripts from a bundle

**Before:** `main.c` built `GetApplicationDirectory() + "../tools"` in two places (the supervisor's start and the
backup's `tools_dir`), which no `.app` bundle has.

**Now:** `src/app/tools_dir.c`, `tools_dir_resolve(app_dir, out, len)`: `$SCRIBODO_TOOLS_DIR` when set and not empty
(used as given); else `<binary dir>/../Resources/tools` when `services.sh` is there and executable (the binary in
`Scribodo.app/Contents/MacOS`); else `<binary dir>/../tools` (the source tree, `make run`), which is also the path
written when nothing is found, so the supervisor's "couldn't run …/services.sh" names a place. Returns whether
`services.sh` was found. Both main.c sites call it (two one-line changes and an include).

**Test:** `test_tools_dir` in `tests/test_services.c`: a bundle layout, a source-tree layout (with and without the
trailing slash), an empty layout (false, the tree path), the override over a bundle, an override with no script
(false, as given), an empty override (ignored), a buffer too small.

Not done here: building a bundle (GAP-002) — the resolver only makes one possible without touching main.c again.

**Gates (item 4 tree):** `make test` exit 0; `make analyze` exit 0; `make sanitize` exit 0.

## Item 5 — the variables documented

`tools/services.sh` opens with a usage block: each command in a line, then every variable the services scripts and
Compose read, with its default (pointing at `tools/services.defaults` rather than repeating its values), and the two
the scripts set for themselves (`SCRIBODO_SERVICES_LOCKED`, `SCRIBODO_MARIADB_SERVER_FLAGS`).
`test_variables_documented` in `tests/test_service_defaults.py` fails if a `SCRIBODO_*` name read by `services.sh`,
`services_lib.sh` or `docker/compose.yml` is missing from the block, or the block names one nobody reads (mutation:
deleting the `SCRIBODO_LOGS` line fails it). README.md is not edited by this stream; the rows to add are in the
stream's report (the six from F6 plus `SCRIBODO_SERVICES`, `SCRIBODO_NATIVE_FRESH` and `SCRIBODO_TOOLS_DIR`, which the
table also lacks).

**Gates (item 5 tree):** `make test` exit 0; `make analyze` exit 0; `make sanitize` exit 0.

## Item 6 — the moved MariaDB tag (F13), and one release on both paths

**What changed upstream**, from the registry API only (auth.docker.io token, `registry-1.docker.io/v2/library/mariadb`
manifests and config blobs; no pull). Both digests are OCI indexes over amd64, arm64/v8, ppc64le and s390x.

| | pinned `sha256:d1891ede…` | tag now `sha256:5e73baf8…` |
|---|---|---|
| `MARIADB_VERSION` (config env, every platform) | `1:11.4.13+maria~ubu2404` | `1:11.4.13+maria~ubu2404` |
| `GOSU_VERSION` | 1.19 | 1.19 |
| `org.opencontainers.image.version` (label / annotation) | 11.4.13 / 11.4.13-noble | 11.4.13 / 11.4.13-noble |
| mariadb-docker revision (annotation) | `bdfe641466a5312bb97d06a1a4e1fb411c49e6b6` | same |
| base (`org.opencontainers.image.base.*`, arm64) | `ubuntu:noble` `sha256:19cad596…` | `ubuntu:noble` `sha256:c9237665…` |
| created (arm64 config) | 2026-10-26T23:29:47Z | 2026-11-11T23:34:22Z |
| arm64 manifest | `sha256:9640785a96dd…` | `sha256:7bbca77eb05b…` |
| layers | 8 | 8, all different digests |

Labels are otherwise identical. Reading: the Official Images rebuild of the same Dockerfile revision and the same
MariaDB package on a refreshed `ubuntu:noble` base (a routine base-image rebuild, 2026-11-11), not a different server
release. Re-pinned `docker/compose.yml` to `mariadb:11.4.13@sha256:5e73baf8179b29f115101bb102763a47a017361f3e9792daccba6646c3ab1532`
(the only tracked file that records the digest outside the audit's prose). Not run: the image itself (no Docker);
the review is of provenance (same revision, same package version), not of the base layer's contents.
`make dependency-report` after the re-pin: exit 0, both images "verified", "All pins and remote checks completed
successfully".

**Native version: exact, not series.** `tools/services.defaults` gains `SCRIBODO_MARIADB_VERSION=11.4.13`;
`native_need` parses `mariadbd --version` and requires that release (exit 3, "MariaDB at … is 11.4.12, not 11.4.13,
the release the store is pinned to (brew pin mariadb@11.4 keeps it)"); `test_mariadb_pin_is_one_release` fails if the
Compose image tag and the file disagree. Reason: what CI and the rehearsal test is the native server, what the
dependency report checks is the image; with the series check the two could be different releases and the report
would speak for neither. The cost, accepted: a `brew upgrade` to 11.4.14 stops the store until the pin is reviewed and
moved (with item 1 the app says exactly that, on the badge and in the status). Homebrew today: stable 11.4.13,
installed 11.4.13_1, not pinned; the installed binary parses to 11.4.13, so the author's store is unaffected.
**Recommendation for the author:** `brew pin mariadb@11.4`.

**Tests:** `tests/test_services.c` adds 11.4.12 (refused by name) to the 10.11.9 case; the Docker-shim test's fake
`mariadbd` reads the version from the defaults file.

**Gates (item 6 tree):** `make test` exit 0; `make analyze` exit 0; `make sanitize` exit 0; `make dependency-report` exit 0.

## Item 7 — spawned tools get 0-2 and nothing else

**Before:** `backup.c:107` spawned the backup pipeline with only `POSIX_SPAWN_SETPGROUP`, closing just its own pipe
ends, so every other descriptor the app held without close-on-exec reached docker/gzip; `server.c:330` ran
`popen("ps -p N -o args=")`, a shell child with the whole table. The supervisor already set
`POSIX_SPAWN_CLOEXEC_DEFAULT` (AUDIT-004 recheck).

**Now:** one helper, `src/core/child_process.c`:
- `child_pipe()`: both ends above 2 and close-on-exec in the app, so no other spawn inherits them;
- `child_spawn()`: stdin `/dev/null`, stdout/stderr on the given descriptors or `/dev/null`, explicit file actions
  (`adddup2`, or `addinherit_np` when a descriptor is already in place), `POSIX_SPAWN_CLOEXEC_DEFAULT`, optionally its
  own process group;
- `child_process_args()`: a process's arguments from the kernel (`sysctl KERN_PROCARGS2`), no child at all.
The backup pipeline's `run()` and the supervisor's `run_services()` spawn through it (behaviour otherwise unchanged:
same group-kill cleanup, same pipe draining); `reap_pidfile()` in `server.c` reads the pid's arguments with
`child_process_args()` instead of `popen` (the llama-server spawn already had `CLOEXEC_DEFAULT` and is unchanged).

**Test:** `test_child_descriptors` in `tests/test_services.c`. The test binary is also the child: `--list-fds`
prints its open descriptors ("fds 0 1 2") on stdout and stderr. With a file and both ends of a pipe held open
*without* close-on-exec (as the register's flock descriptor was), it spawns the lister through the helper (stdout
pipe), through the supervisor (a fake `services.sh` that execs the lister; the reason line must be exactly
"fds 0 1 2"), and through `backup_project` (a fake `backup.sh` that does the same; the error line). Then
`child_process_args` on a spawned `/bin/sleep 30` (exact arguments, truncation to the buffer, its own process group)
and on the same pid after it is reaped (false). Mutation: without `POSIX_SPAWN_CLOEXEC_DEFAULT` all three spawn checks
fail (the held descriptors appear in the child). `test_llm`'s `reap_orphan` and `orphan_after_owner_kill` exercise the
new pid check end to end and pass.

**Gates (item 7 tree):** `make test` exit 0; `make analyze` exit 0 (0 warnings, 89 files); `make sanitize` exit 0.

The coordinator warned that an outside `pkill -f "clang --analyze"` around 16:50 may have killed analyze runs. Every
analyze recorded here exited 0 and printed its "0 warnings across N files" line; a clang killed by a signal exits
nonzero, which this rule turns into a failure, so none of them was cut short.

## End-of-stream gates (tree `29cfc1a`)

- `make dbtest`: exit 0 (twelve suites, every one "0 failures").
- `make servicestest`: exit 0, run once after `pgrep -f servicestest` found no other run, with the main checkout's
  pinned Qdrant (`qdrant 1.20.0`, `QDRANT_BIN`/`SCRIBODO_QDRANT_BIN` pointed at it so the worktree downloaded
  nothing). Steps 1-9 ok; step 10 skipped, Docker not running. Step 1 now waits for Qdrant through `qdrant_curl`, so it
  is the live evidence that Qdrant 1.20 accepts `Authorization: Bearer` with the plain API key.

**For the coordinator, after merge:** this stream changed the native `rootsql`/`dump_db`/`load_dump` transport (item
3). The shim test covers both modes' commands, but a real MariaDB runs them only in `backuptest`, `uitest-db`
(`testdb create`/`drop`) and the restore path, which this stream was told not to run. Those are the gates that
confirm item 3 against the real server.

# Red-team remediation (same day)

The coordinator's red team reviewed `remed/services` and found six problems. Each fix below is one commit. Gates for
each: `make test`, `make sanitize`, and `make analyze` only when `pgrep -fl "clang --analyze"` showed nothing else
running; `make dbtest` and `make servicestest` once at the end.

## RT1 — Compose's required variables made direct callers pass vacuously (medium)

Item 2 made `docker/compose.yml` require `SCRIBODO_MARIADB_SERVER_FLAGS` and the ports (`:?`). Only
`services_lib.sh` exports them, so every bare `docker compose` elsewhere failed at parse time:
- `servicestest.sh`'s three "containers stopped" checks read empty output and passed;
- its cleanup `down -v` failed silently;
- `backuptest.sh`'s Docker-mode shell-escape probe (`! incontainer 'test -e …'`) was true whenever Compose failed.

Fix: both scripts call Compose through the library (`bash -c '. tools/services_lib.sh; compose …'`).
`containers_down PROJECT` is true only when Compose exited 0 *and* listed nothing. The cleanup reports a failed
removal. The probe must print `absent`, so a probe that cannot run fails the step. Tests added to
`tests/test_services_docker.sh`:
- a static check that no script, Makefile recipe or workflow calls `docker compose` except through the library. Run
  against the pre-fix scripts, it lists all five sites (`backuptest.sh:67`, `servicestest.sh:41,183,190,236`);
- a check that `up` runs Compose with the server flags exported (the shim records what Compose would have
  interpolated).

**Gates (RT1):** `make test` exit 0; `make analyze` exit 0 (no other analyze running); `make sanitize` exit 0.

## RT2 — the MariaDB version policy (medium; the coordinator's design decision)

Item 6's exact-release check is replaced by the coordinator's policy, which lives in
`services_lib.sh:mariadb_version_check`:
- the pinned release passes silently;
- a newer patch of the same series passes, and `up` ends with `warning: MariaDB 11.4.14 is running; the store is
  pinned to 11.4.13 (…)` on stderr and exit 0;
- another series, an older patch, or a version that cannot be read is refused with exit 3, saying what is needed.

`tools/services.defaults` now writes `: "${SCRIBODO_MARIADB_VERSION:=11.4.13}"`, so the pin can be overridden (the
defaults test's parser accepts that form; the header documents the variable among the user-settable ones). The
exit-code contract gains one line: on success, a last stderr line starting `warning: ` is for the author.
`service_supervisor_warning()` returns it without its prefix, only after a success. `main.c` shows it in the status
line once per distinct warning (one field outside the unions, five lines).

**Tests:**
- `tests/test_services_docker.sh`, through the library with a fake `mariadbd`: the pin passes silently; pin+1 passes
  with a warning naming both versions; pin−1, 11.5.0, 10.11.9 and an unreadable version are refused with 3 and the
  reason; the override moves the floor in both directions.
- `tests/test_services.c`, the real script: 11.4.12 and 10.11.9 are refused (3, by name); 11.4.14 gets past the
  version check (the missing stand-in Qdrant then refuses with 4); an override to 11.4.15 refuses 11.4.14. The
  supervisor's warning accessor returns the line after a success, and nothing after a plain success or a failure.
- The real Homebrew binary passes silently (rc 0, no warning).

Not exercised: the warning at the end of a whole successful native `up` with a newer patch. That needs a real newer
server; the pieces it joins (the check, the final `echo`, the accessor) are each tested.

**Gates (RT2):** `make test` exit 0; `make analyze` exit 0; `make sanitize` exit 0.

## RT3 — the trap remapped only 3–6 (low)

A command failing under `set -e` with its own 7 (curl) or 75 passed straight through, which the app would read as
"lock held". Now the EXIT trap turns every code above 2 into 1 unless `leave` set it. A real 75 can only come from
`lockf`, the outer process, which the trap is not part of. The lock wait is overridable
(`SCRIBODO_SERVICES_LOCK_WAIT`, default 300 s, listed in the header) so the test can make lockf give up quickly.

**Test** (`tests/test_services_docker.sh`, on the real script):
- `status` in docker mode, with the Compose shim failing 4, 7 and 75: the script exits 1 each time;
- a store whose lock another `lockf` holds: `down` with a one-second wait exits 75 (the holder is stopped by PID).

With the old 3–6 trap restored, the 7 and 75 cases fail. The 4 case passed either way, being inside the old range.

**Gates (RT3):** `make test` exit 0; `make sanitize` exit 0; `make analyze` skipped (another stream's `clang --analyze` was running). RT3 changes no C source, which is all analyze covers.

## RT4 — tests that would have passed with the fixes reverted (low)

- **server.c's popen.** `tests/test_spawn_hygiene.py` (in `make test`) is the static check the coordinator offered as
  the alternative. Comments and string literals are stripped first. It forbids `popen`, `system`, `fork`, `vfork` and
  the `exec*` family in `src/`, and allows `posix_spawn` only in `child_process.c` and `server.c`, each of which must
  contain `POSIX_SPAWN_CLOEXEC_DEFAULT`. Run on the pre-AUDIT-004 `server.c`, it finds the `popen`. I chose this over
  driving `reap_pidfile`: `test_llm`'s reap tests pass with either implementation, because both read the right
  arguments; the difference is only what the child inherits, which a static rule pins.
- **Shell literals.** `test_service_defaults.py` now also forbids Qdrant's port in shell files and any `:-<port>`
  default in the product files (Compose keeps 6333 only as the container's own port). The pre-AUDIT-004
  `services_lib.sh` line `${SCRIBODO_QDRANT_PORT:-6333}` is caught, and a test case pins that.
- **Docker guard.** `tests/test_services_docker.sh` puts a `docker` that prints "the real docker was reached" and
  exits 97 at `$HOME/.docker/bin`, the throwaway HOME's copy of the directory the library puts first on PATH. A check
  confirms that, without the function shim, the library resolves to the guard and gets 97. With the shim's export
  removed, 13 checks fail on the guard, and no real Docker is contacted.

**Stash audit (the coordinator's warning that the stash stack is shared).** During RT1 I ran one bare `git stash`
followed by `git stash pop`, intending to check the old scripts. Afterwards:
- the shared stash list is empty, so no entry of mine was left behind, as there would be had my pop taken another
  stream's entry;
- the pop restored exactly my three RT1 files (`tests/test_services_docker.sh`, `tools/backuptest.sh`,
  `tools/servicestest.sh`), which are what `78f2049` commits, so no other stream popped mine.
Nothing odd found. No stash has been used since; old file versions are read with `git show <sha>:<path>`.

**Gates (RT4):** `make test` exit 0; `make sanitize` exit 0; `make analyze` skipped (another stream's analyze was running). RT4 changes tests only, no C source.

## RT5 — tools_dir_resolve's partial path, ignored results, the bundle layout (low)

`tools_dir_resolve` now returns true only when `services.sh` is in the folder it writes. On every failure `out` is
empty, including when the buffer is too small, which used to leave a truncated path that could name another folder.
`main.c` checks the result at all three uses:
- the store start: no script → a status message once ("Scribodo's scripts weren't found beside the app …; set
  SCRIBODO_TOOLS_DIR"), the badge sentence "the services script isn't installed", and a probe only, so services
  started by hand are still used;
- backup and restore: a clear status, and nothing is spawned.

**Bundle layout:** `src/app/tools_dir.h` lists what a bundle must hold, beside the binary in `Contents/MacOS`:
- `Contents/Resources/tools/{services.sh, services_lib.sh, services.defaults, backup.sh, restore.sh}`;
- `Contents/Resources/docker/compose.yml`;
- `Contents/Resources/bin/qdrant`;
- MariaDB stays Homebrew's.

The library resolves `here` to `Resources`, so the Compose file is found there. It now looks for Qdrant at
`$here/bin/qdrant` before the source tree's `build/qdrant/qdrant` (`SCRIBODO_QDRANT_BIN` still wins). That makes
the layout consistent rather than only documented.

**Tests:**
- `tests/test_services.c`: an empty layout and a script-less override give false with empty `out`; a buffer one
  byte short gives false and empty, and the exact size succeeds.
- `tests/test_services_docker.sh` builds the documented bundle from the tracked files and checks: the header's list
  matches; the bundle's library resolves `here` to `Resources` and Qdrant to `Resources/bin/qdrant`; its `services.sh
  up` in docker mode runs on the bundle's own `compose.yml`; and in native mode it finds its Qdrant and the given
  MariaDB, then stops at the credentials guard (2) before starting anything. No daemon was left running (checked with
  pgrep).

**Gates (RT5):** `make test` exit 0; `make sanitize` exit 0; `make analyze` exit 0 (0 warnings, 89 files).

Correction to the RT3 and RT4 skips: `pgrep -fl "clang --analyze"` also matched other streams' *shell command lines*
that merely contain that text, so "another analyze is running" may have been false both times. My gate now matches
only a process whose executable is clang (`pgrep -fl '^[^ ]*clang(-[0-9]+)? --analyze'`). This analyze run, on a
tree that includes RT3 and RT4, is their analyze result too.

## RT6 — the reason cut mid-character (minor)

The reason was cut byte-wise at 239, and `service_supervisor_describe` cut again wherever its buffer ended. Either
could split a UTF-8 sequence, which the app then draws. Both now cut with `utf8_clip` (`src/core/utf8.c`). The line
is sanitised in full first and then clipped at a character boundary. The description is composed in full and then
clipped to the caller's buffer. `main.c` copies the description into `store_why`, a buffer of the same size, so no
third cut happens.

**Test** (`reason_utf8` in `tests/test_services.c`):
- a stderr line of 200 "é": the reason is valid UTF-8, 238 bytes (119 whole characters, where a byte cut at 239
  would split the 120th);
- the description into its normal buffer and into every size from 1 to 63: valid each time;
- a line of "€": 237 bytes.

Mutation: with byte-wise `snprintf` restored in both places, the case fails at the reason, the description and 16
of the small-buffer sizes.

**Gates (RT6):** `make test` exit 0; `make analyze` exit 0 (0 warnings, 89 files); `make sanitize` exit 0.

## Red-team remediation: end gates (tree `14bcad8`)

- `make dbtest`: exit 0 (12 suites, each "0 failures").
- `make servicestest`: exit 0, run once after `pgrep` found no other run, with the main checkout's pinned Qdrant.
  Steps 1-9 ok; step 10 skipped, Docker not running. The new version check (RT2), the wider trap (RT3) and the
  bundle-aware Qdrant lookup (RT5) all ran on real daemons here: the installed 11.4.13 passed silently, and every
  `up` exited 0.
- Still for the coordinator after merge: `backuptest` (now also RT1's corrected shell-escape probe in docker mode,
  which needs Docker to mean anything), `uitest-db`, and the restore path, which run item 3's root operations on a
  real server.

# Verification follow-up (same day)

The coordinator's verification closed RT1–RT6. It found one new medium issue and four small gaps; one commit for
each group.

## V1 — an expansion error under `set -u` exited 0 (medium; dates from bcd8aec)

Bash runs the EXIT trap after a failed `${n:?}` or an unset variable with `$?` = 0, and the trap kept 0. Reproduced on
the pre-fix script (`git show 14bcad8:tools/services.sh`, a throwaway store, docker mode, Docker shimmed):
`testdb create` printed "line 434: 3: name" and exited 0; `dump` printed "line 453: 2: database" and exited 0. An
unset-variable bug in `up` would have read as success to the app.

**Fix:**
- Every intended exit goes through `leave`, which marks the run finished: the contract codes, the refusals, the
  rollback, `testdb sql`, and the end of the script (which maps a non-zero status from a command `set -e` exempts to
  1).
- The trap is now `[ -n "$finished" ] || exit 1`: any other way out, a stray code or an expansion error, is 1. Bash's
  own message stays the last stderr line, which is the reason the app shows.
- The `${3:?name}` and `${2:?database}` checks are explicit usage errors (`usage`, exit 2). `testdb` with an action
  other than create, drop or sql is now a usage error too; it used to fall through to drop.

**Tests** (`tests/test_services_docker.sh`, the real script):
- `testdb create`, `testdb drop`, `testdb`, `testdb bogus NAME`, `dump` and an unknown command each exit 2 with the
  usage line and run nothing;
- a copy of the real scripts with an unset variable planted in `status` and in docker `up` exits 1 for each, with
  bash's error on stderr and Compose never started.

**Gates (V1):** `make test` exit 0; `make analyze` exit 0; `make sanitize` exit 0.



## V2 — the small gaps

- **(a) Version format.** A release is now three numbers of at most nine digits with no leading zeros, for both the
  pin and what `mariadbd` reports. A 20-digit patch used to make `[ -lt ]` fail with "integer expression expected",
  which read as "not older". Decision on `11.4.013`: refuse it, not normalise it. `mariadbd` never prints a leading
  zero, so it is a misread or a mistyped pin. Tests: a 21-digit, a 10-digit and a zero-padded patch are refused (3); a
  zero-padded pin is refused as "not a release".
- **(b) Removed, not stopped.** `servicestest.sh`'s `containers_down` lists with `ps -aq`, so a stopped container that
  still exists is not "down". This only runs in step 10, with Docker.
- **(c) The Compose check, wider.** It now matches `docker compose` with any spacing, `docker-compose`, and docker
  through a variable (`$DOCKER compose`, `"${docker_bin}" compose`). The test asserts the pattern catches each of those
  five spellings and ignores the library's own `compose` and `docker info`.
- **(d) Spawn hygiene, wider.** Names are matched without requiring `(`. Banned as words anywhere: `popen`, `vfork`,
  `forkpty`, `daemon`, `wordexp`, the `exec*` family including `execvP`, `dlsym`, `dlopen`, and `NSTask` (in `.m`).
  `system` and `fork` are ordinary variable names in `src/` (a chat message's system part, a forked project), so they
  are banned as a call, a pointer taken (`= system;`, `&fork`) or a macro alias. Samples tested: the verification's
  seven (`#define RUN popen`, `= popen;`, `dlsym(…, "system")`, `wordexp`, `forkpty`, `execvP`, `NSTask`), the
  generic names in pointer and macro form, and four false-positive guards (the real variable uses, a comment, a
  string). The pre-AUDIT-004 `server.c` is still caught (`popen`).

**Gates (V2):** `make test` exit 0; `make analyze` exit 0; `make sanitize` exit 0.

## Verification follow-up: end gates (tree `adf4f0d`)

- `make dbtest`: exit 0 (12 suites, each "0 failures").
- `make servicestest`: exit 0, run once (no other run found), with the main checkout's pinned Qdrant: 17 checks ok
  across steps 1-9 (every native `up`/`down`/`purge` now ending through `leave`); step 10 skipped, Docker not running.

# Rebased onto main 467ac7f (the main.c split)

`git rebase main`, resolved per commit; one commit per item kept. Five commits conflicted only in `src/main.c` (and
the Makefile's TESTS list in item 1). Each was resolved by taking main's file and re-applying the commit's own hunks
where that code now lives:
- the App fields (`services_headline`, `services_warning`) in `src/app/app_internal.h`, outside the unions;
- the offline badge sentence in `src/ui/layout.c`;
- the supervisor's reason, warning, store-start and backup/restore tools-dir checks in `src/main.c`, which still holds
  `store_try_later`, `tools_dir` and the poll loop.

Every file this stream owns outside those three is byte-identical to the pre-rebase tree (`git diff` against it is
empty, apart from main's own new file). The spawn-hygiene and environment checks pass against main's new files.

**Gates on the final tree:** `make test` exit 0 (including `test_env_writes`, `test_env_symbols`,
`test_spawn_hygiene`, and the services tests); `make analyze` exit 0 (0 warnings, 99 files; no real clang analyzer
was running); `make sanitize` exit 0; `make dbtest` exit 0 (12 suites, 0 failures); `make servicestest` exit 0, run
once (17 checks ok across steps 1-9, step 10 skipped without Docker).

# With Docker running: the migration's restore order (2026-11-16)

Branch `remed/services-docker`, from main `a2e02aa`. The logs are in `build/audit004-services-docker/`.

The coordinator ran `SCRIBODO_SERVICESTEST_DOCKER=1 make servicestest` on main for the first time with Docker up.
"A replacing migration whose load fails leaves the native store exactly as it was" failed with
`mariadb-dump: … 1045 Access denied for user 'root'@'localhost'`, then "port 33078 is taken … the native services did
not come back". Both parts of the coordinator's diagnosis are confirmed:

1. **Harness.** `scribodo-st-h_mariadb` was created 2026-11-15T02:30:44Z by an interrupted run (with
   `scribodo-st-h_qdrant`; no `scribodo-st-h` container remained). The rehearsal's cleanup removed volumes only for
   stores whose secrets folder existed in the current run, so every later step h reused that volume, whose root
   password belonged to a deleted secrets folder.
2. **Product.** `migration_restore` brought the native services back (when they had been running) *before* stopping
   the containers the migration had started; the two share their ports. Any failure after `compose up` in a migration
   with native running (the dump denied, an incomplete dump, a failed load) left the author's native store down.

## A — the restore order, and the mismatch said by name

`migration_restore` now:
1. reloads and stops the native store;
2. stops the containers this migration started, and reports it if that fails (it used to ignore the failure);
3. only then brings back the native services that were running;
4. then the containers, if they had been running.

`migration_rollback` and the `leave 2` refusal both go through it. Every rollback path was checked against the new
order: `compose up` fails, the credentials check, the dump, the incomplete dump, `compose down`, native `up`, the
table count, the native dump, the load and the schema check.

Before dumping, the migration now checks root against the running containers. On "Access denied" it says "the
containers' root password does not match the secrets folder (their volume was initialised under other credentials);
nothing moved" and rolls back. That is the realistic author case: credentials regenerated or rotated after the volume
was made.

**Rehearsal case, step 10 (store i, ports 33079/6349):**
- Setup: initialise a volume under one root password; give the secrets folder another; start the native store and
  write a probe row; then run `SCRIBODO_MIGRATE_REPLACE=1 migrate-from-docker`. Every setup step is checked; a failed
  setup fails the case and says so.
- Assertions: exit 1; the mismatch named; "both stores are as they were"; both native services running; the probe
  reads "native original"; the containers removed (`ps -aq` empty); the remembered mode not docker.
- Main's `services.sh` with this case (`A-before-fix-servicestest.log`): FAIL, "exit 1, running 0, read back []",
  with the same "port 33079 is taken … did not come back" as the coordinator's run.
- The fixed script (`A-after-fix-servicestest.log`): all of step 10 ok, exit 0, and only the author's two volumes left
  afterwards.

## C — the stale volumes

Removed by exact name only: `docker volume rm scribodo-st-h_mariadb scribodo-st-h_qdrant` (exit 0;
`C-stale-volumes.log`). The author's pre-move volumes `scribodo_mariadb` and `scribodo_qdrant` (created
2026-11-04T20:32:49Z) are untouched, before and after.

## B — the rehearsal's Docker hygiene

`fresh_project X` removes, by exact name, the containers labelled with Compose project `scribodo-st-X`, its network,
and its two volumes `scribodo-st-X_mariadb` / `_qdrant`. It refuses any name that is not a single letter, so it
cannot reach `scribodo_mariadb` or `scribodo_qdrant`.
- Each Docker store (e, g, h, i) calls it before its setup.
- The cleanup calls it for all four whether or not this run created their secrets folder (the old cleanup skipped
  any store without one, which is how the 2026-11-15 volume survived).
- Every Docker step's setup (the `up`, the `rootsql` heredocs, h's `down`, native `up` and native `rootsql`) is now
  checked: a failed setup fails its case and prints "setup failed: the case below is not what it claims", so a broken
  setup can no longer pass for the case under test.

**Verified with a deliberate stale state.** `B-planted-stale.log`: a `scribodo-st-h` store was brought up through
`services.sh` with a throwaway secrets folder on 33078/6348, taken down, and its secrets folder deleted, which is
exactly an interrupted run's leftovers. Then two `SCRIBODO_SERVICESTEST_DOCKER=1 make servicestest` runs back to back
on this tree (`B-servicestest-run1.log` and `-run2.log`, with `.exit` files):
- both exit 0, 22 checks ok each (all 10 steps, five Docker cases), no "setup failed", no cleanup complaint;
- after each run only `scribodo_mariadb` and `scribodo_qdrant` exist, with no rehearsal container or volume
  (`B-after-run{1,2}.state`).

## backuptest in Docker mode

This exercises RT1's corrected shell-escape probe, which only means something when the loading client is inside a
container. Steps:
1. A throwaway Docker store: `SCRIBODO_SECRETS` in a scratch folder, `SCRIBODO_SERVICES=docker`,
   `SCRIBODO_COMPOSE_PROJECT=scribodo-bt`, ports 33063/6335. `tools/services.sh up` exited 0, both containers healthy
   (`backuptest-docker-up.log`).
2. `IOConsoleLocked` was false. `make backuptest` exited 0 with 18 checks ok, including "a dump with a shell escape is
   refused", whose probe now has to print `absent` from inside the container (`backuptest-docker.log`, `.exit`).
3. The store was taken down and purged with `SCRIBODO_PURGE_CONFIRM=1`: `scribodo-bt_mariadb` and `_qdrant` removed,
   the author's two volumes still present (`backuptest-docker-teardown.log`).

## Gates on the final tree (`d61db9a` plus this journal and EVIDENCE.md)

- `SCRIBODO_SERVICESTEST_DOCKER=1 make servicestest`: exit 0, twice in a row (above, B).
- `make test`: exit 0, including `test_services_docker`, the evidence check (every citation above archived) and the
  observer yardstick (pp 717, mars 348, heights 948, byte-identical). The first attempt exited 2 only because the new
  worktree had no `build/manuscripts`; the test novels were linked in from the main checkout, and the rerun passed.
- `make sanitize`: exit 0, after the same link (its first attempt failed the same way).
- `make analyze`: exit 0 (0 warnings, 102 files; no real clang analyzer was running).
- `make dbtest`: exit 0 (12 suites, 0 failures; no `test_db` or `test_crash_points` process was running).
- `make backuptest` in Docker mode: exit 0 (above).

The evidence is in `scribodo-evidence-2026-11-16-docker.tar.gz`; `docs/EVIDENCE.md` gives its manifest and SHA-256.
