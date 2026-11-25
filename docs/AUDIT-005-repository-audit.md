# AUDIT-005: persistence review and dependency checks

**Date:** 2026-11-25  
**Baseline:** `69b98abf1ecf86c560b435a209327175c182eab1`  
**Status:** open findings; no product fixes made in this audit  
**Previous audit:** [AUDIT-004](AUDIT-004-repository-audit.md)

**Implementation follow-up (2026-11-25):** COMPOSITION-001 C01 implements F1/F3 corrections with red/green
regressions and passing full headless tests, analyzer, ASan/UBSan, TSan and app build.
See [commands, retained logs and exclusions](../journal/composition001-progress.md).
REDTEAM-025 additionally passed a real scripted file-save refusal/retry UI case and isolated DB/crash/DB-UI
gates; this is not a real-model or IME/accessibility claim. F2 and D1/D2 remain open.
The findings and line numbers below describe the original audit baseline, not the corrected working tree.

## Scope and limits

This was a targeted correctness, persistence and security review, not an exhaustive end-to-end validation.
Source inspection covered document operations, save/recovery, revisions, chat persistence and lifecycle,
store routing, backup orchestration, remote transport, font-file handling, and CI configuration.
Small isolated C probes reproduced the three defects below against the product's compiled objects.
Commands, probe source and observations are in [the journal](../journal/audit005-findings.md).

The database integration, crash, UI, real-model, offline and sanitizer suites were **not run**.
Passing mechanical tests below does not establish model quality, real author usability or the absence of other defects.
Line numbers refer to the baseline above.

## Verification

| Command | Exit | Observation |
|---|---:|---|
| `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test` | 0 | Existing headless suites passed; archive check ran and the public-domain novel yardsticks matched byte-for-byte; the variables permit missing inputs but did not skip present inputs. One branch-specific experiment test skipped. Description corrected during REDTEAM-025 from the retained log. |
| `make analyze` | 0 | Zero warnings across 113 C/Objective-C source files |
| Isolated audit probe | 0 | Reproduced F1, F2 and F3; exit 0 means the diagnostic ran, not that these behaviors are correct |
| `make dependency-report` | nonzero (`make`: 2) | Two OSV advisories for the pinned llama.cpp commit; MariaDB image tag moved |

## Findings

### F1 — failed file-backed chat saves bypass the unsaved-chat guard (P1)

**Status:** open. **Evidence:** reproduced state plus inspected UI call sites.

- `src/llm/chat.c:260–270`: `chat_window_info()` returns immediately when `s->windowed` is false.
  It does not examine `ChatMessage.save_failed`.
- `src/app/chat_workflow.c:549–554`: `unsaved_chat()` relies exclusively on that window's `unsaved` field.
- The guards for New chat (`:563`), opening another chat (`:706`) and the quit flow
  (`src/main.c:1225`) use this result.

The file-backed path is not a database history window. A message whose persistence failed can therefore
remain in memory with `save_failed=true` while the UI's guard reports no unsaved chat.
The probe returned `save_failed=1 window.unsaved=0`.

**Consequence:** after a failed file write, replacing or closing the in-memory conversation can lose text
without the intended unsaved-chat confirmation. The probe did not automate a real UI disk-failure session.

**Remediation direction:** define persistence state independently of history-window membership.
Every unsaved-chat guard must include failed appends in either backend. Preserve that protection until a
successful retry or explicit discard.

**Regression gate:** exercise a failed file-backed append, attempt New/open/quit, and require protection;
then retry successfully and require the protection to clear without inventing a database sequence.

### F2 — distinct long filenames share a revision directory (P1)

**Status:** open. **Evidence:** reproduced through the public revision APIs.

`src/app/revisions.c:29–42` maps document basenames to a `char safe[200]`, keeping at most 199 bytes.
The truncated/sanitized name is the history directory identity. It is not collision-resistant.
Import (`src/app/document_workflow.c:318–327`) can retain names longer than 199 bytes, and external files
can have such names regardless of the New document prompt's 200-byte limit.

Two distinct 203-byte names, each consisting of the same 199 `x` characters followed by `A.md` or `B.md`,
both successfully recorded revisions. Listing A returned two revisions, and loading its newest revision
returned **B's text**.

**Consequence:** document histories merge. An author can be offered another document's text for restoration,
and source snapshots no longer identify a unique document. Original manuscript files were not overwritten by
the probe.

**Remediation direction:** use stable document identities or a collision-resistant encoding of the complete
basename; provide a migration strategy for existing histories. Coordinate the change with rename,
history set-aside, import/export and source resolution, rather than changing only `doc_dir()`.

**Regression gate:** distinct long basenames must have disjoint history and snapshot resolution.
Also cover collisions introduced by sanitization and rename/import round trips.

### F3 — successful file-backed retries leave messages eligible for another append (P2)

**Status:** open. **Evidence:** reproduced through `chat_store_resave_failed()`.

- `src/app/chat_store.c:706–733`: a successful retry calls `chat_mark_saved()` only for a nonnegative sequence.
- `src/app/project_store.c:596–605`: file-backed appends leave the sequence at `-1`.
- `src/llm/chat.c:288–298`: clearing `save_failed` is coupled to establishing a database sequence.

Retrying one marked message twice returned success twice and persisted two copies. After the first retry
the original message still had `save_failed=true`. The existing test at `tests/test_llm.c:659–690` explicitly
notes that file-store marks stay but does not test repeated successful retries.

**Consequence:** repeated retry invocations duplicate an already-saved message. This is a demonstrated API
defect, not a claim that the ordinary UI retries file-backed messages on every frame. Its automatic retry
call site (`src/main.c:2081–2083`) is gated on service/store availability and runs before project adoption.

**Remediation direction:** acknowledge a successful append separately from assigning a database sequence.
Do not use a fabricated sequence or accidentally convert a file-backed conversation into a database window.

**Regression gate:** a second retry after a successful file-backed append must append zero messages;
a failed retry must retain the mark.

## Dependency follow-up

### D1 — pinned llama.cpp commit matches two published advisories

**Status:** requires security triage; exploitability through Scribodo was not established.

The committed dependency is `5851a8e505f4` (`b12111-5-g5851a8e50`). The repository's dependency report
queried OSV by that commit and returned:

- [CVE-2026-133710](https://osv.dev/vulnerability/CVE-2026-133710): integer overflow in the `hrm_text`
  GGUF loader leading to a heap out-of-bounds write. The OSV record identifies the fix as
  `50518b64c6f2191a116d2760042f106c4b3387d7`, released in `b12556`.
- [CVE-2026-134540](https://osv.dev/vulnerability/CVE-2026-134540): input validation of
  `grammar_triggers[].type` in `common_sampler_init`; the OSV record identifies the fix as
  `9f26fe11c73c7e74a5e99e7f1b6370ad8e1cb22d`, released in `b12405`.

Both records were independently read through `https://api.osv.dev/v1/vulns/<CVE-ID>` on the audit date.
Pinned model hashes and a private authenticated model-server socket reduce the relevant attack surface;
they are not a substitute for checking reachability and applying appropriate fixes.
Upgrade or backport with the BF16, model-request and real-model regression gates. No exploit was run.

### D2 — MariaDB's tagged image no longer resolves to the pinned digest

**Status:** requires dependency-maintenance review; no compromise established.

For `mariadb:11.4.13`, the registry lookup returned
`sha256:a8992b5e5404d11e03c54a31d0c1bdea3fd116d283aa6d1a627551f29045309b`,
while the committed pin was
`sha256:5e73baf8179b29f115101bb102763a47a017361f3e9792daccba6646c3ab1532`.
Qdrant's digest verified. A moved tag does not by itself mean the pinned image is unavailable,
vulnerable or compromised. Investigate why it changed before changing the pin.

## Return-to-work order

1. F1: protect failed file-backed messages from unintended discard.
2. F2: isolate revision identities, including migration/source compatibility.
3. F3: acknowledge successful file-backed retries without database-window side effects.
4. D1/D2: complete dependency triage and rerun the dependency gate.

Keep this dated report as the original observation. Record remediation commits and their verification here
when fixes are authorized; do not mark these items resolved on the strength of the pre-existing green tests.
