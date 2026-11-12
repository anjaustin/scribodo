# GAP-004, cycle 1: kill points and the migration rows

**Date:** 2026-11-11. **Tree:** `fcf30ba` plus the changes recorded below.
**Direction:** "Let's continue with the P0s in order until complete."

## The item

GAP-004 is the fourth of the P0 working order: deterministic crash and
recovery coverage at the store's transitions. Every restart test in the tree
models a crash by editing state; none kills a process mid-transition. The
first row named for it was the migration: AUDIT-003 F1 found migration 015
not idempotent because its `ADD COLUMN` lacked `IF NOT EXISTS`, and the test
that caught it deletes receipts to stand in for a crash between DDL and
receipt. A real death at every statement is the measurement that test
approximates.

## The harness

`src/core/crash_point.h`: `crash_point("area.step")`, compiled to nothing in
the product. `tests/crash_point.c`: the test implementation, armed once at
load from `SCRIBODO_CRASH_POINT` (`area.step` or `area.step#N`), kills the
process with `SIGKILL` on the selected hit; the counter is atomic so a point
hit from a worker thread is counted. `tests/test_crash_points.c` (`make
crashtest`): the driver, which forks and execs itself under each point and
hit on a fresh throwaway database until a child finishes without dying, then
recovers in its own process and checks the invariant. The admin helper that
creates throwaway databases moved from `tests/test_db.c` to `tests/db_admin.c`
so both binaries share it. The environment variable is read in `tests/`, not
in `src/`; the product has no flag for this.

Two points in `db_migrate_upto`: `migration.statement` after each DDL
statement runs (the process dies holding the statement's effect and no
receipt for its migration) and `migration.receipt` after each receipt is
inserted.

The invariant checked on every death: `db_migrate` in the next process
completes to schema 15; `schema_migrations` has exactly 15 rows, one per
version; the schema fingerprint (every column with its type, nullability,
default, key and collation; every index column with uniqueness, type and
prefix; every constraint and check clause; every table's engine and
collation, from `information_schema`, in a fixed order) equals that of a
database migrated once without interruption; and a further `db_migrate`
changes nothing.

## Measurement

`make crashtest` (`build/crash/crashtest.log`, `build/crash/matrix.tsv`):

| Point | Deaths | Schema version found at death | Recovered to 15 | Receipts 1..15 | Schema identical |
|---|---:|---|---:|---:|---:|
| `migration.statement` | 80 | 0 (14 deaths), 1, 2 (26), 3, 4, 5 (7), 6, 7, 8 (9), 9 (5), 10 (3), 11 (4), 12 (5), 13, 14 | 80 of 80 | 80 of 80 | 80 of 80 |
| `migration.receipt` | 15 | k after receipt k, 1 to 15 | 15 of 15 | 15 of 15 | 15 of 15 |

864 checks, 0 failures, 31 s for 96 throwaway databases (one reference and
95 deaths). Every child died of `SIGKILL` at the selected hit and no other
signal or exit was seen. Migration 002 has 26 statements and migration 008
nine, which is why the version-at-death column clusters there.

One correction to the driver during the run, not to the product: the first
run failed one check of 864 because the driver required the schema at death
to be below 15, and a death after the fifteenth receipt leaves exactly 15.
The receipt point now checks the version equals the receipt count.

What the migration rows establish: a process can die after any one of the 80
statements or 15 receipts of a fresh migration, and the next process produces
the clean schema with one receipt per version. What they do not: data
preservation under a mid-migration death on a populated database, which the
replay test in `tests/test_db_core.c` holds by deleting receipts (migrations
003, 009, 010, 011, 012 with rows present); seeding the driver's databases
before the kill is the next refinement of this row, not a new row.

## Gates

`make test` 95,885 C checks and 161 Python tests, 0 failures; `make dbtest`
18,000 checks, 0 failures; `make analyze` 0 warnings (88 source files); `make sanitize`
95,885 checks, 0 failures, 0 diagnostics; `make tsan` 48,549 checks, 0
failures, 0 diagnostics; `make crashtest` 864 checks, 0 failures. Every
target's exit status 0 (`build/crash/gates-*.log`, `build/crash/crashtest.log`).

## Next rows

The matrix (`docs/GAP-004-interruption-matrix.md`) lists the open rows in
order: document save, store writes, legacy import marker, fork cleanup, disk
full and read-only. Each starts by naming its points in the product code.
