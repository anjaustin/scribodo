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
