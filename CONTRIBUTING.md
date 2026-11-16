# Contributing to Scribodo

Scribodo is pre-alpha and developed on one Mac. Issues and pull requests are
welcome. This page covers what a change needs before it lands, and the
working rules behind the project's records.

## Building and testing

Build and run as in the [README](README.md#quick-start). The test targets and
what each covers are in [TESTING](docs/TESTING.md). In short:

| Change touches | Run before you commit |
|---|---|
| Anything | `make test`, `make analyze`, `make sanitize` |
| Threads (chat, observer, supervisor, workers) | `make tsan` |
| The store, migrations or the importer | `make dbtest crashtest uitest-db` (needs the services up) |
| The UI | `make uitest` (needs an unlocked console) |
| `tools/services.sh` or `tools/backup.sh` | `make servicestest backuptest`; for migration changes also `SCRIBODO_SERVICESTEST_DOCKER=1 make servicestest` with Docker running |

- **Gate on the exit status,** not on a summary line. A commit or a written claim that a target passes needs that target's exit status and its log.
- **Run database gates one at a time.** `dbtest`, `crashtest` and `uitest-db` share the one local MariaDB; run concurrently they hang until the watchdog kills them.
- **Never point a test at a real store.** Tests use throwaway databases, Compose projects and secrets folders; `tools/services.sh` takes `SCRIBODO_SECRETS` and the port variables for that ([CONFIGURATION](docs/CONFIGURATION.md#services)).
- **Test data is public domain.** The test novels come from Project Gutenberg ([MEMORY-021](docs/MEMORY-021-real-manuscript.md)). Never commit or test with an author's own manuscript.

CI runs the hosted suites on every push and pull request. The trusted
acceptance suite runs on a self-hosted Mac, only for branches in this
repository, so a pull request from a fork gets the hosted checks and a
maintainer runs acceptance after review.

## How changes are made

- **One step per commit.** Each commit is one change with its tests and its evidence, and the gates pass on the committed tree itself, not just on the working tree.
- **Fix the software, not the prompt or the data.** A model error is answered with a structural fix (a check, a gate, a better input), and the software repairs its own data. Wording changes to a prompt are the last resort.
- **Measure model-facing changes.** Any change to a prompt or a model request is measured on the real model, old against new, before it ships. A replay that tests a judge or gate rule logs every reply with its ground truth, so the rule can be scored offline.
- **Red-team, then remediate.** Larger changes get an adversarial review that tries to break them. The findings, the fixes and the verification are recorded (the `docs/REDTEAM-*` series).

## Experiments and flags

- Every experiment flag is a field of `Experiments` in `src/core/experiments.h`, read once at startup, with its measured default stated beside it. No other code reads one from the environment.
- Only `src/app/startup_env.c` writes the environment, before the first thread starts (`tests/test_env_writes.py`, `tests/test_env_symbols.py`).
- Every `SCRIBODO_*` variable has a row in [CONFIGURATION](docs/CONFIGURATION.md) or in TESTING's harness table (`tests/test_config_documented.py`).
- **Delete nothing, only archive.** A concluded or failed experiment stays in the tree behind its flag, or moves into the measurement harness; it is never simply deleted ([MEMORY-015](docs/MEMORY-015-effective-memory-contract.md) §5).
- A measurement states every experiment arm it ran with. `tools/experiment_arms.py` pins the arms a tool leaves unset.

## Schema changes

A new column or migration touches more than a grep finds:

- every result-column-count check on the query's rows (`db_rows_has_columns` and `ncols ==`), including the one before a row loop;
- the legacy importer's `INSERT` (`src/app/store_import.c`) and the file store's JSON key (`chat_store.c`);
- positional initialisers of the affected structs;
- the schema pins in `tests/test_crash_points.c` (`LATEST_SCHEMA`, the receipt counts, `MIGRATION_STATEMENTS`) and `tests/test_db_core.c`;
- the counts in [GAP-004](docs/GAP-004-interruption-matrix.md) and [LANDSCAPE](docs/LANDSCAPE.md) row 8.

Then run `make dbtest crashtest`.

## Documentation and evidence

- **Living docs** (README, [USER-GUIDE](docs/USER-GUIDE.md), [SERVICES](docs/SERVICES.md), [CONFIGURATION](docs/CONFIGURATION.md), [TESTING](docs/TESTING.md), [LANDSCAPE](docs/LANDSCAPE.md), the registers) describe the tree as it is; update them in the same commit as the change.
- **Dated reports** (the numbered `docs/` series) keep their point-in-time figures. Correct a wrong figure in place and say so; don't rewrite history. The [documentation map](docs/README.md) groups them.
- **Journals** (`journal/`) hold each step's measurements and the commands that produced them. Figures in docs cite their journal entry.
- **Evidence** cited by its `build/` path must be in an archive listed in [EVIDENCE](docs/EVIDENCE.md); `tools/evidence_check.py` (in `make test`) checks the archives' actual contents.

## Code

C11 with `-Wall -Wextra -Wpedantic -Wshadow`; `make analyze` must report
nothing. Vendored libraries are pinned submodules under `third_party/` and are
not edited in place. Keep a new module's tests in the narrowest existing test
binary.

## Reporting a security issue

See [SECURITY](SECURITY.md). Please don't open a public issue for one.
