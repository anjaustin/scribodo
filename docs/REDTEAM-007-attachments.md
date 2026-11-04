# Red-team review 007: chat attachments and chat rename

**Date:** 2026-11-04
**Scope:** commit `55f6979`:
- several documents attached to a chat by reference;
- the documents each message was sent with;
- fair sharing of the token budget;
- the chip row and the attach menu;
- chat rename.

**Method:**
- **Independent reviewer:** fake-model UI scripts, plus C probes calling `chat_build_request` directly.
- **Fixes:** each has a regression test. Mutation checks were validated by a successful build and a changed binary.

**Result:** 12 findings. Four could send the model text the author didn't choose to send. All are fixed.

## Findings

| # | Sev | Finding | Fix | Verified by |
|---|-----|---------|-----|-------------|
| 1 | H | **Reopening a chat restored documents the author had removed.** A message sent with nothing attached had no record, so an older message's set came back and was sent again. | Only the newest user message counts. "Nothing attached" is recorded as `"docs":[]`. | `attachments.scribo` |
| 2 | H | **A tampered chat log could attach any file,** such as `../other/secret.md` or a chat log, and send it to the model. | Only documents the Explorer lists for this project (or the open one) can be attached or read for a request: no `/`, hidden files or control characters. | `attachments.scribo` (tampered log: only the listed `b.md` survives) |
| 3 | H | **Reopening a chat after a file name was reused attached the wrong document.** Renames were applied regardless of when they happened. | A message's names follow only renames made after the message (`renames_resolve_since` with its time). | `attach_many.scribo` |
| 4 | H | **Opening a document in a chat that hadn't started discarded the author's chosen attachments.** | The open document is followed only until the author changes the attachments. | `attachments.scribo` |
| 5 | M | **Non-UTF-8 files were sent as raw bytes,** which would make the request invalid JSON. | Decoded with replacement characters and re-encoded, as the editor does. | code review (scripts can't write raw invalid bytes) |
| 6 | M | **Edit requests recorded every attached document**, although only the edited one is sent. | Edit requests pass, and record, only the edited document. | `attachments.scribo` (reopen after an edit shows only it) |
| 7 | M | **A single long-named document showed as "+1 more", with no name and no ×.** | The first chip always shows, shortened to fit. The rest overflow to a clickable "+N more" that opens the menu. | `attach_many.scribo` |
| 8 | M | **With overflow, "+ Attach" covered the Send button.** | Room for "+N more" and "+ Attach" is reserved before chips are placed, and "+ Attach" is clamped to the row. | `attach_many.scribo` (five attachments, then Send sends) |
| 9 | M | **The attach menu showed only 16 documents, and could open above the window.** | With more documents, the last item is "Other document…", which prompts for a name (listed documents only). Menus are clamped to the screen. | `attach_many.scribo` |
| 10 | M | **Renaming a chat to a very long title reported success but changed nothing.** Lines over 4 KB were skipped when listing. | Titles are kept to 150 bytes on a character boundary. Lines of any length are read. | code review |
| 11 | L | **Names with line breaks split into two records, and a title could forge the request's document framing.** | Names with control characters aren't listed. In request headers, quotes in titles become apostrophes and line breaks become spaces. | `test_llm: multi_doc_request` |
| 12 | L | **Polish:**<br>• two messages still mentioned "Include document";<br>• the cut-off notice spoke of one document;<br>• a single attachment was called "the author's current document";<br>• title and header tokens weren't counted against the 60% share;<br>• labels could be cut mid-character;<br>• the field `include_doc` was unused. | All fixed. | code review |

## Verification

- **`make test`:** 10,009 checks. `make sanitize` and `make tsan` are clean.
- **`make uitest`:** 20 scripts, including the new `attach_many.scribo` (9 checks). `attachments.scribo` grew from 19 to 28 checks.
- **`make uitest-real`:** passes.
- **Mutation check:** 9 of 9 real fixes caught. Two candidates were dropped as redundant second layers:
  - Recording "none" explicitly: reopening reads only the newest message, which gives the same effect.
  - Clamping "+ Attach": the space reservation already keeps it inside the row. Removing the reservation itself is caught.

## Remaining risks

- **The attachment list is per window, not saved with the chat until the next message.** Reopening shows what the last message was sent with, not edits made since without sending.
- **Chat order follows the log file's modification time,** so renaming a chat moves it to the top. The MariaDB store keeps `updated_at` explicitly.
