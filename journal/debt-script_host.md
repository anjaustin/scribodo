# Debt: the compiled-in script host, decided

**Date:** 2026-11-12. **Tree:** `2fd2866` plus the observer split.
**Direction:** "Let's wrap up the loose ends. 100/100. Methodically." The
third item of TECHNICAL-DEBT's worker-monolith entry (AUDIT-003 F5): decide
whether the UI test interpreter stays in the product binary, and list it under
intentional tradeoffs, or moves behind a build flag.

## What it is

`src/app/script.c` (1,006 lines; `script.h` 140) interprets the `.scribo`
files under `tests/ui/`: it drives the real application through a `ScriptHost`
of 17 adapters in `main.c` (`host_*`: element rectangles, status text, the
explorer and findings text, memory settlement, store restart, drop, open) and
checks expectations frame by frame, taking screenshots. It is compiled into
`build/scribodo` (`Makefile`, `APP_SRCS`) and activated only by the
`SCRIBODO_SCRIPT` environment variable, which `make uitest`, `make uitest-db`
and the hosted acceptance workflow set; without it `app.script` is NULL and the
38 script-conditional sites in `main.c` take their product branch. Measured
footprint on this tree: `script.o` is 21,889 bytes of text and 97,001 in all;
the application binary is 7,376,328 bytes (so the host is about 1.3% of it);
30 of its symbols are in the binary.

## The two ways

**Behind a build flag.** A `-DSCRIBODO_SCRIPT_HOST` build for the test suites
and a product build without it. The product binary loses 1.3% and an
interpreter that reads a file named by the environment. The cost: the UI
suites and the hosted acceptance then test a binary that is not the one an
author runs. Every `.scribo` expectation about layout, input, store transitions,
the observer and the chat would hold for the test build and be assumed for the
product build; the 38 `#ifdef` sites would be a second configuration to keep
compiling and to reason about on every change to `main.c`.

**In the product binary, inert without the variable.** The tested binary is the
shipped binary, which is the point of a scripted UI suite over the real
application. What an author could do with the host: nothing without setting
`SCRIBODO_SCRIPT` in the app's environment, and a person who controls the
environment of the process already controls the process (`SCRIBODO_MODEL`,
`SCRIBODO_STORE`, `SCRIBODO_DB_NAME` and the experiment flags are read the same
way). The host reads the script file, the script directory and the shot
directory from the environment and writes screenshots and a state file under
them; it opens no network connection and reads no path a script does not name.

## Decision

The script host stays in the product binary, listed under intentional
pre-alpha tradeoffs in TECHNICAL-DEBT with this record as its reason. The
decision is revisited once, at GAP-002, when a signed release build exists:
if that build's hardening review wants the interpreter out, the switch is the
`APP_SRCS` line and a guard on the 38 sites, and the acceptance workflow must
then run both builds, the scripted one for the suites and the release one for
a smoke run, so the shipped binary is still the one exercised. Until a release
build exists there is no second binary to protect, and splitting the build now
would only make the suites test something else.

The worker-monolith entry's three items are done: `build_in_worker`
(`journal/answer-cycle4_builder.md`), `run_job` and `observer_job_find`
(`journal/observer-cycle1_split.md`), and this decision. The entry closes.

## Hosted verdicts

The decision shipped with the observer split in `2aede34`: CI and Trusted
acceptance both passed (runs 43321958181 and 43321958154).
