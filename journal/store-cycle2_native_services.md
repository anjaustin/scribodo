# Store cycle 2: the services on bare metal (2026-11-14)

The author's decision, after the default-path measurement showed the chat
model's context falling from 32K to 8K with the reranker resident: "Docker is
way too expensive for the services we are running." Native MariaDB and Qdrant
become the recommended default on resource-constrained systems like the 16 GB
M4; Docker containers stay as an installation option.

## What the VM costs, measured

The app's own available-memory formula (`sysmem_available`: total minus
wired, compressor and non-purgeable anonymous pages), read from `vm_stat`
with Docker Desktop running its two healthy containers, after quitting it, and
after relaunching it:

| state | available |
|---|---|
| Docker up, containers running | 7.00 GB |
| Docker quit | 8.94 GB |
| Docker relaunched, containers not yet started | 9.36 GB |

So the virtual machine costs about 1.9 GB on this Mac while the containers
themselves used 428 MiB (MariaDB) and 374 MiB (Qdrant) by `docker stats` at
the time. The hypervisor process shows a 36 MB footprint; the guest's RAM is
counted as anonymous memory elsewhere, which is why the fit sees it. For
comparison, the same services run natively:

| service | at rest | peak under `make dbtest` |
|---|---|---|
| MariaDB 11.4.13 (Homebrew), 128 MB buffer pool, 32 connections | 104 MiB | 168 MiB |
| Qdrant 1.20.0 (release binary) | 52 MiB | 69 MiB |

The chat model needs 5.2 GB at a 32K context and the launch fit had 4.0 GB to
give it with the reranker resident; 1.9 GB more brings 32K back with room to
spare, and no model precision is touched (the alternatives, 8-bit KV or
weights, would have broken the validated BF16 contract).

## What was built

- `tools/services_lib.sh`, sourced by `services.sh`, `backup.sh`,
  `restore.sh` and `backuptest.sh`: the mode (`SCRIBODO_SERVICES`, else the
  mode the last `up` wrote beside the secrets, else native when its binaries
  are present), and `rootsql`, `dump_db`, `load_dump` and the index cleanup
  with a native and a docker body each, so every script that reached the
  database through `compose exec` now works in both modes.
- `tools/services.sh` native mode: `up` initialises a data directory under the
  secrets folder on first use (`mariadb-install-db`), sets the generated root
  password on the three loopback root accounts, creates the `scribodo` user
  and database as the container's first start did, starts `mariadbd` with the
  compose file's options on 127.0.0.1 and a Unix socket, and starts Qdrant on
  loopback HTTP with the API key passed by environment and telemetry off;
  `down`, `status`, `logs`, `purge` (refuses the author's own store unless
  asked), `rotate-root`, `testdb` and `rootsql`/`dump` for the other scripts;
  `migrate-from-docker` dumps the containers' store (the dump is kept beside
  the secrets), stops the containers, starts the native services and loads it.
- `make qdrant`: the pinned 1.20.0 Apple Silicon release binary, SHA-256
  checked, beside `llama-server` under `build/`.
- The hosted acceptance job runs on the native services too (`make qdrant`,
  its own secrets folder under the runner's temporary directory, ports beside
  the author's, `purge` at the end), so Docker Desktop need not run on this Mac
  at all; `SCRIBODO_SERVICES=docker` by hand selects the compose project.

## The move, and what it bought

The native mode first ran on a scratch secrets folder and alternate ports
(`build/observer/native-services/`): `make dbtest` 18,006 checks, 0 failures;
`make crashtest` 133 rows, 0 failures, in 543 s against the containers' 64 s,
because every commit's fsync now reaches the disk (the durability the
containers could not promise, REDTEAM-022 R9); `tools/backuptest.sh` clean.
One defect of the new script found there: Qdrant's pid file held the script's
own pid (a subshell's `$!`), so `down` reported Qdrant stopped and left it
running; fixed, Qdrant is started in the script's shell.

Then the author's store moved: `tools/services.sh migrate-from-docker` dumped
the containers' `scribodo` database to
`services/migration-20261114-131311.sql.gz` (kept), stopped the containers
(the volume untouched), started the native services on the default ports and
loaded the dump: 1 project, 24 messages, 1 document, schema 18. Resident after
the move: MariaDB 112 MiB, Qdrant 53 MiB.

The launch fit on the settled machine with Docker Desktop quit and all three
model servers starting (`build/ui/fit-native2`): "a 32K context needs about 5.3
GB and 5.2 GB is free for it, so the model runs with 24K for now"; the chat
server's slot is 24,576 against 8,192 an hour earlier with the containers'
VM resident, and 32,768 before the reranker joined. One more gigabyte freed
(Docker Desktop's own app processes were still running, 158 MiB, beside the
browser) gives 32K back entirely.

| launch | servers | chat context |
|---|---|---|
| Docker VM resident, two model servers | embed | 32K |
| Docker VM resident, three model servers | embed, rerank | 8K |
| native services, Docker quit, three model servers | embed, rerank | 24K |

## What the faster store showed

The suites against the migrated default services (`build/observer/
native-services/gates-*.log`): `make test` 95,766 C checks and 193 Python
tests; `make dbtest` 18,006; `make crashtest` 133 rows (543 s); the backup
rehearsal clean; `make uitest-db` 279 of 281, the two failures both in the
in-app restore script. Sampled frame by frame, the status line read "Restored
as …" at the frame of the click and "This project is now kept in the project
store" one frame later: on the native store the restored project's import
finished within a single frame, so the author would never have seen the
restore reported, and the script's second wait began after the import's
message had expired. The fix is in the app, not the script: a status that
follows another within two seconds joins it (`set_status_after`), so the line
reads "Restored as … · This project is now kept in the project store"; the
script passes unchanged, and would under Docker too.

## Gates

On the tree with the native mode, the migrated store as the default services
and the status fix (`build/observer/native-services/final-*.log`): `make test`
95,766 C checks and 193 Python tests, 0 failures; `make uitest` 409, 0; `make
uitest-db` 281, 0; `make analyze` 0 warnings across 87 source files; `make
sanitize` 95,766, 0; `make tsan` 48,339, 0; earlier the same evening on the same
services `make dbtest` 18,006, 0, `make crashtest` 133 rows, 0 failures, and the
backup rehearsal clean; every exit status 0. Hosted: recorded below when the
first native acceptance run reports.
