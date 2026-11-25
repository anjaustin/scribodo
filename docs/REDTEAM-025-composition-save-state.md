# REDTEAM-025 — composition save-state boundary

**Date:** 2026-11-25. **Reviewed checkpoint:** `b0f81a3`.
**Scope:** COMPOSITION-001 C01, its callback/adoption/retry callers, history guards and tests.
**Status:** R1–R6 remediated; final verification passed. Not a numerical safety certification.

The author requested a baseline commit, adversarial review, remediation/verification iterations, then a
second commit. The checkpoint includes the earlier audit, architecture and component research as durable
context; this review is not a revalidation of every upstream research claim or all of Scribodo.

## Findings and repairs

| ID | Severity | Counterexample | Repair and regression |
|---|---|---|---|
| R1 | P1 | The retry helper's first or second array allocation fails: no messages are copied, `saved` remains true, `*ok` reports success and the caller silently returns. | Remove batch allocation; count the obligation without allocating, then snapshot/append/acknowledge one marked message at a time. Fault-inject both initial allocation failures and every allocation along adoption and retry. |
| R2 | P1 | Adoption binds the project before allocating/appending. Allocation failure or a refused append leaves the unsaved suffix unmarked; file-mode replacement guards see no failure and retries select nothing. | Mark the complete adoption obligation before any fallible preparation; acknowledge only the successful prefix. Shared, testable `chat_store_adopt`; refused-path and allocation-sweep tests verify protection and eventual exact message count. |
| R3 | P1 | `on_message_done` cannot duplicate the message body and returns without attempting persistence or marking the message failed. The callback is consumed; file-mode guards still consider it saved. | On preparation failure, mark the existing project-bound message failed and expose the in-memory-only status. Persistence snapshots now own all optional strings as well as content; allocation tests verify that required metadata is never silently dropped. The app-level early-return branch is source-reviewed and app-built; it is not claimed to have direct callback fault injection. |
| R4 | P1 | User append fails, but the assistant callback subsequently appends successfully. Retrying the user afterward reverses the stored conversation order. | `chat_store_save_finished` defers later finished messages behind an earlier failed append. Sequential retries cannot acknowledge a suffix past a failed prefix. Tests fail the user save, restore routing, attempt the assistant save and verify exact restored order/metadata. DB and file paths are exercised separately. |
| R5 | P2 | After failed file saves are correctly guarded, no file-mode user action calls retry: the old retry caller is tied to database-service recovery. Restored disk access alone cannot unblock the chat. | Offer **Explorer → CHATS → Retry saving chat** for an idle, project-bound unsaved chat. It retries only marked saves, never submits composer text or regenerates an answer. Add a synthetic read-only-directory UI script covering failure, open/New guards, explicit recovery and reopen with no duplicate messages. |
| R6 | P2 (evidence) | Earlier descriptions said archive/novel gates were skipped because the skip environment variables were set. Those variables only permit absence; the retained logs show present inputs were checked. | Correct the audit table and composition journal from the actual logs. Preserve the commands and successful observations; do not downgrade the gates or alter the golden data. |

R1–R5 were inherited gaps in the touched persistence boundary, exposed by broadening the C01 review beyond
the two original reproductions. None is a reason to migrate the database or add a persistence framework.
The repair centralizes acknowledgement/snapshot/order policy without a new dependency.
R6 is a reporting correction rather than a production-code defect.

## Adversarial coverage

- File failure vs normal unknown sequence; real DB acknowledgement vs file sentinel; invalid sequence
  acknowledgement and invalid message index; DB-window unsequenced protection.
- Failed messages cannot be trimmed even if they carry a known sequence; acknowledgement invalidates the
  cached trim decision. A changed failure state increments the history-publication version.
- Refused retry, no marked messages, repeated successful retry, first/second snapshot allocation failure,
  one-shot/permanent failures at all measured allocation sites, saved-prefix/refused-suffix recovery.
- Exact ordering, original content, attachments, provider label, retrieval-source string and message flags.
  Storage timestamps retain the existing **append-time clock** semantics; copying a `ChatMessage.ts` does
  not redefine the backend clock contract.
- A busy worker does not trigger batch adoption/retry; the finished-user callback can safely snapshot while
  the worker streams the later reply. Transcript replacement/persistence are explicitly UI-thread operations;
  this API is not advertised as a multi-writer transcript protocol.
- Negative controls deliberately remove failure reporting, adoption marking and file acknowledgement.
  These must produce assertion failures, not compilation errors, crashes or timeouts.

## Iterations and evidence

Retained raw logs are under [journal/composition001/redteam](../journal/composition001/redteam/).
Exact commands, exit statuses and acceptance exclusions are in the
[composition handoff](../journal/composition001-progress.md).

1. Original checkpoint plus the retry-allocation regression: **exit 1**, two assertion failures. This
   proves R1; the remaining findings additionally have source control-flow traces and new focused tests.
2. First green attempt was **not green**: the tests assumed turn timestamps survive append unchanged.
   Source inspection of the existing backend clock contract disproved that assumption. Tests now check
   valid stored timestamps while retaining exact payload/metadata assertions; no backend clock policy changed.
3. The next allocation run exposed test-fixture pollution: new adoption trials added chats to the shared
   directory used by pre-existing list-count tests. Move the new trials to their own synthetic project,
   rather than weakening the old count assertions.
4. Negative controls passed: both unmodified controls green and all **13** deliberately broken variants
   rejected by assertions. The source-specific allocator sweeps passed with **3,484** checks. Full file-mode
   UI testing passed, including **14** new recovery assertions. The full isolated DB suite passed with the new
   save/sequence/window test.
5. Final documentation gate caught a slash-separated prose phrase mistaken for an evidence path; reworded
   it without adding an exception. A later parallel test/sanitizer run exposed shared yardstick output paths.
   Preserve those failures, document serial scheduling in `AGENTS.md`, and rerun serially. Gold data is unchanged.
6. Final serial tests and ASan/UBSan passed: **5,161 chat checks**, **3,484 persistence-fault checks**, zero
   failures; public-domain yardsticks matched byte-for-byte. Final TSan passed with the worker-overlap test.
   Static analysis reported **zero warnings across 113 sources**. App build passed. Isolated crash testing
   passed **1,711 checks over 151 interruption cases**; the full file UI suite passed **510 assertions across
   29 scripts**, and the serial isolated DB UI suite passed **317 across 16 scripts**. Both isolated services
   were stopped afterward. Exact commands and retained failures are in the handoff.

## Limits

No “100/100” score can establish absence of defects. The acceptance claim is limited to the tested and
reviewed boundary, with no known unremediated finding in R1–R6 after the final verification pass.
The original AUDIT-005 F2 revision collision and dependency D1/D2 are separate queued work, not repaired here.
An uncertain write/commit may already have reached storage despite reporting failure; exactly-once recovery
requires stable append identities and remains outside C01's acknowledged-success correction. The explicit
retry UI and guide do not promise otherwise. Real-model quality, native IME and accessibility are unchanged
and not established by these tests.
