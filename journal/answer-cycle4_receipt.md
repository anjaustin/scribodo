# Answer cycle 4, the P0 list: the receipt and trace contract

**Date:** 2026-11-11. **Tree:** `67d1b39` plus the changes recorded below.
**Direction:** "Start on the receipt contract."

## The item

The register's P2 entry (TECHNICAL-DEBT, "answer receipts and trace lines are
an unversioned contract", AUDIT-003 F4): the verified pipeline writes four
marker lines into the reply's stored provenance (`memory_sources`) and four
trace lines to stderr; `tools/answer_pipeline_review.py` parses them with six
regular expressions and, since the window rule, the product itself reads the
gate line back (`chat_receipt_refusal`). No document defined the format, no
test covered the round trip, and nothing checked the receipt against the
store's 16 KiB bound on the column.

That last point is a defect, not only debt. The store refuses a message whose
`memory_sources` exceeds `CHAT_MEMORY_SOURCES_BYTES` (file store, database
store, import, and a CHECK constraint on the column). The receipt was built
without regard to that bound: the pipeline's span text and the gate's two free
fields (the model's claim about its own reply, the first misquoted phrase)
were appended at whatever length they came. A reply whose receipt passed
16 KiB would have been refused by the store and lost from the record. The
largest receipt in any recorded run is 938 bytes, so this has not happened;
the bound is now enforced where the receipt is built, so it cannot.

## The change

`src/llm/answer_receipt.h` is the contract: every receipt line and every trace
line, the field rules, the refusal rule the composer window applies, the
bound, and where each is written and read. `src/llm/answer_receipt.c` holds
the four writers. `chat.c` no longer formats any receipt or trace text; it
calls `answer_receipt_append_gate`, `answer_trace_pipeline` and
`answer_trace_gate`, and `answer_receipt` moved here from the pipeline.

Two rules of the format changed, both for the readers:

- The free-text fields (`answer=`, `misquoted=`) carry no newline (newlines
  become spaces) and are cut at `ANSWER_RECEIPT_FIELD_MAX` (1,024 bytes) on a
  UTF-8 boundary. A newline in a claim would have broken the line-based
  parsers and the review tool's expressions alike.
- A free-text field runs to the line's final ` ---`. The review tool's
  expressions took the first ` ---`, so a claim containing that sequence
  would have been cut short; the two expressions now anchor to the end of
  the line. On every recorded receipt the two readings are the same.

The bound. `answer_receipt` never exceeds `ANSWER_RECEIPT_PIPELINE_MAX`
(16 KiB less the room the gate's two lines can need at most, 2,304 bytes):
header lines are always complete, span lines are cut on a UTF-8 boundary to
fit, first span first. `answer_receipt_append_gate` keeps the whole receipt at
or under `CHAT_MEMORY_SOURCES_BYTES`, cutting the claim first and the
misquotation second, down to empty; it leaves the receipt unchanged only when
even the empty-field lines do not fit, which the pipeline bound prevents.

The tests are the round trip the register asked for. `test_answer_pipeline`
(`test_receipt_contract`) builds three results (two sources answered; one
source withheld with a misquotation, a newline and ` ---` in the claim; no
candidates and an abstention), writes receipt and trace through the product's
writers, and compares byte for byte with `tests/fixtures/answer_receipt/`
(`receipt-pass.txt`, `receipt-withheld.txt`, `receipt-abstained.txt`,
`trace.log`; `SCRIBODO_WRITE_FIXTURES=1` regenerates them after a deliberate
change). It then drives the bound: spans of 32 KiB in three-byte characters
cut between characters to exactly the pipeline maximum; a 32 KiB claim and
misquotation appended to that stay within the store bound; 120 bytes of room
empties the claim and keeps what fits of the misquotation; 10 bytes of room
leaves the receipt untouched. `test_answer_receipt_contract` (Python) parses
the same four files with the review tool's expressions, asserts every field,
and applies the refusal rule in Python to the three receipts (false, true,
true), matching `chat_receipt_refusal` on the same bytes in C.

## Measurement

The product under its default and traced flags must behave identically on a
recorded run, now including the receipts and the trace. The measure is the
second novel's boundary script, run live on an instrumented build of this
tree (`build/answer-live-receipt`, run `mars-pipe7`) against the previous
run on the previous tree (`mars-pipe6`, same environment:
`SCRIBODO_ANSWER_PIPELINE=verified`, `SCRIBODO_MEMORY_TRACE=1`,
`SCRIBODO_MEMORY_EXCERPTS=0`, seed 42). Unit ids are per database (each run
has its own disposable test database) and the score lines list the candidate
pool in index order, which varies between tied candidates run to run, so the
comparison masks unit ids and timing fields and compares score pools as sets;
on the two previous runs (`mars-pipe5` against `mars-pipe6`) that measure
reports 10 of 10 requests, 10 of 10 replies, 10 of 10 receipts and 27 of 27
trace lines identical, which is its calibration.

| | mars-pipe6 (before, 325 s) | mars-pipe7 (after, 323 s) |
|---|---|---|
| Composer requests sent (`request-0..9.json`) | 10 | **10 of 10 byte-identical** |
| Composer replies (`content-0..9.txt`) | 10 | **10 of 10 byte-identical** |
| Stored receipts (`memory_sources` of the 10 replies), unit ids masked | 10, largest 938 bytes | **10 of 10 identical**, largest 938 bytes |
| Trace lines (`ANSWER_PIPELINE`, `_SCORES`, `ANSWER_GATE`), unit ids and timings masked | 27 | **27 of 27 identical** |

The run is under `build/product-memory-use/mars-pipe7`, its capture under
`build/window/capture-mars-receipt`, the comparison script and its output at
`build/window/receipt-compare.py` and `receipt-compare.txt`, the launch log at
`build/window/receipt-mars-run.log`; all to be archived with the next evidence
tarball. The script exit code (1) is the same as every earlier run of this
script: the two source-delivery checks the second novel's record discusses
(`journal/answer-cycle4_mars.md`), unchanged here.

Gates: `make test` 95,885 C checks and 151 Python tests, 0 failures (38 checks
and 6 tests more than the previous tree); `make dbtest` 18,000, 0;
`make analyze` 0 warnings across 88 source files; `make sanitize` 95,885
checks, 0 failures, 0 diagnostics; `make tsan` 48,549 checks, 0 failures, 0
diagnostics (`build/window/receipt-gates-*.log`, each target's exit status 0).

## What remains of the register entry

Nothing of the remediation. The entry stays as a record. The receipt format
is now a stored contract with a C writer test and a Python reader test over
one fixture; the next format change starts by editing the header and
regenerating the fixture, and both tests then say what else must follow.
