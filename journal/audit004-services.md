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
