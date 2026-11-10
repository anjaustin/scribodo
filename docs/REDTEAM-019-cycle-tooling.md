# REDTEAM-019: adversarial review of the audit remediations and cycle tooling

**Date:** 2026-11-09
**Reviewed tree:** the uncommitted changes on `6c243d5`: the AUDIT-002
remediations (`src/llm/http.c`, `tests/test_memory_vectors.c`, `ci.yml`, docs),
the UI script runner fix (`src/app/script.c`), and the cycle tooling
(`tools/answer_income_isolation.py`, `answer_citation_binding.py`,
`answer_boundary_bind.py`, `answer_boundary_review.py`, `answer_backend_compare.py`,
`answer_product_run.py`, the `--timeout` option, the boundary fixture and UI
script, their tests and the two cycle journals).

## Method

Every changed line was read against the question "what does this let a server,
a reply or a test input do that it could not before, and what number did it
change?" Suspicions were measured before being written down; corrections were
followed by mutation controls that reintroduce the defect into a generated copy
(tracked sources untouched, `build/redteam019-mutations/`) and must fail the
same test the correction passes. The full target matrix was rerun at the end.

## Findings and remediations

### R1 — the Content-Length presize let a server reserve 128 MiB for one byte (P2, fixed)

The AUDIT-002 decoder fix sized the whole-body buffer from `Content-Length` up to
the 64 MiB body limit. Doubling growth overshoots that bound: a response declaring
`Content-Length: 67108864` and delivering one byte made the decoder allocate
**134,217,728 bytes** immediately (`body_cap` after the first byte), where the
previous code allocated 256. The decoder talks only to local sockets, but a
declared length is server-controlled data and the reservation preceded any
evidence the body would arrive.

Remediation: presize only when the declared length is below `PRESIZE_MAX`
(1 MiB); larger bodies grow as they arrive, as before. Measured after: the same
one-byte response reserves **256 bytes**. The delivery-independence regression
gained a second check that a 64 MiB declaration with one byte reserves at most
2 MiB. Mutation controls (vectors fault binary, 16,678 checks):

| Variant | Result |
|---|---|
| unchanged control | passed |
| M1: presize removed (HEAD decoder) | killed, 2 failures: `whole=2 1KiB=5 bytewise=7` |
| M2: presize unbounded (first version of the fix) | killed, 2 failures: `reserved 134217728 bytes for a one-byte body` |

### R2 — quotation-binding labels were matched case-sensitively (P2, fixed; numbers republished)

`tools/answer_citation_binding.py` found the cited source with `Source (\d+)`.
Across all captured replies the labels are written `SOURCE N` **163** times,
`Source N` 112 times and `source N` 4 times, so most citations were treated as
uncited and their quotations landed in "another source than cited". Measured
before and after the case-insensitive match over the same 43 replies:

| | pieces | bound to cited source | in another source | preview only | no supplied text |
|---|---:|---:|---:|---:|---:|
| before | 220 | 15 | 70 | 34 | 101 |
| after | 220 | **46** | **39** | 34 | 101 |

The unsupported and preview counts, which carry the entry's main claim, did not
move; the bound/other split did, and every figure in `journal/answer-cycle1_income.md`,
`answer-cycle2_boundary.md` and the pq2 section of `answer-bedrock_live.md` was
re-derived from the regenerated outputs (`build/answer-citation-binding.json`,
`build/answer-boundary-capture/review.json`). A unit test now mixes `SOURCE`,
`Source` and `source` labels. Mutation control M4 (case-sensitive label regex
in a generated copy of the tool) fails that test; the unchanged tool passes.

### R3 — the review sheet duplicated the classifier (P3, fixed)

`answer_boundary_review.py` carried its own copy of the piece-classification
loop, so R2's fix would have applied to one tool and not the other. The
classifier is now one function (`classify`) imported by both; a test asserts the
two paths return identical counts on the same input.

### R4 — the script runner's cut detail: verified over mixed widths (P3, fixed in the reviewed tree)

The UI script runner truncates expectation details at 1,200 bytes and had split a
multibyte character, which produced the invalid byte that crashed the evidence
runner. The fix walks back to a UTF-8 boundary. Harness over 100 cut positions
with 2-, 3- and 4-byte characters in the value: previous helper **11 invalid
outputs**, fixed helper **0** (`build/redteam019-mutations/results.json`, variants
`m3_head_show` and `control_current_show`; sources beside it).

### R5 — the boundary run depended on an untracked runner (P3, fixed)

The UI script's header and the journal pointed at `build/product-memory-use-run.py`,
which is not in the repository. It is now tracked as `tools/answer_product_run.py`
(with the tolerant log read), lints clean, and the script header names it.

### R6 — fixture invariants had no test (P3, fixed)

`tests/test_answer_boundary_fixture.py` (in `make test`) checks that the fixture
covers the six required kinds, that no boundary question appears in the
seven-question script, that the generated UI script matches the fixture, and
that the binder refuses ambiguous or missing passages.

### Reviewed without change

- `ci.yml` concurrency expression: parses as YAML; its hosted effect is
  observable only after a push and is not claimed.
- `answer_income_isolation.py`: packet verification refuses a tampered history,
  a missing quoted block, a designated unit already present, and a header prefix
  from another document revision (4 tests). The skipped second position arm is
  recorded in the journal.
- `--timeout` on the contract tool: help text now states the measured CPU rate
  (3.6 to 3.8 tokens/s in pq1 and pq2) rather than an estimate.
- AUDIT-002's description of the decoder fix was corrected to the bounded form.

## Final verification

All **17 targets exited 0** on the remediated tree in a measured **782.5 s** of
sequential wall time (`build/redteam019-results.json`, `build/redteam019-<target>.log`).

| Target | Result |
|---|---|
| `all` | build passed, 0 compiler warnings |
| `analyzetest` | 8 Python gate tests passed |
| `test` | **95,593 C checks** in 26 binaries and **73 Python tests** in 13 files, 0 failures |
| `sanitize` | 95,593 C checks and 73 Python tests, 0 failures; 0 ASan/UBSan diagnostics |
| `tsan` | 48,300 checks, 0 failures; 0 ThreadSanitizer diagnostics |
| `uitest` | 398 passed, 0 failed |
| `dbtest` | 18,000 checks, 0 failures |
| `uitest-db` | 281 passed, 0 failed |
| `backuptest` | 38 UI assertions, 0 failed |
| `remotetest` | 7 passed, 0 failed |
| `cstest` | 3 passed, 0 failed |
| `llmtest` | 24 real-model checks, 0 failures |
| `uitest-real` | 35 passed, 0 failed |
| `offlinetest` | 110 script passes plus 24 real-model checks, 0 failed |
| `analyze` | 0 warnings across 83 files |
| `dependency-report` | pins and digests verified; no OSV advisories |
| `tools/test_audit_mutations.py` | 10 killed, 0 survived; both controls passed |

Against AUDIT-002's remediated tree the unit total rose by 2 checks (the lying
Content-Length pair) and the Python tests from 59 to 73 (income isolation,
citation binding and fixture tests). Mutation controls for this review: M1, M2,
M3 and M4 all killed with their unchanged controls passing
(`build/redteam019-mutations/results.json`). Tracked Markdown: 204 relative
links, 0 broken; `git diff --check` clean; `pyflakes` clean over `tools/` and
`tests/`.

**Disposition:** R1–R6 closed. This report records local verification; the
hosted effect of the `ci.yml` change and a hosted run of the decoder fix follow
the next push and are not claimed. The open AUDIT-002 F3 item is registered in
`TECHNICAL-DEBT.md` with its measured baseline.
