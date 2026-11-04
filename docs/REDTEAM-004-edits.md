# Red-team review 004: reviewed AI edits

**Date:** 2026-11-04
**Scope:** the reviewed-edits feature (commit `2630da5`):
- the word-level diff (`src/core/diff.c`)
- edit requests and reply cleaning (`src/llm/chat.c`, `llm_text.c`)
- the review panel (`src/ui/review_view.c`)
- the editor's range APIs (`src/ui/editor.c`)
- the revision store (`src/app/revisions.c`)
- the proposal flow in `src/main.c`: ⌘K, pending edit, proposal, Accept/Reject, Apply to document, History restore

**Method:** Two passes:
- **Author's pass:** a probe program sent real edit requests to LFM3 (`llama-server` with the production flags) and inspected the cleaned replies and their diffs.
- **Independent reviewer:** worked from the code alone, using the fake model and UI scripts, and reproduced four findings as scripts.

Every fix has a regression test. A mutation check, run in a scratch copy, confirmed that breaking each fix fails its test.

**Result:** 17 findings. Eight could change the author's text without approval or put an edit in the wrong place. All are fixed.

## The main fix: a tracked anchor

A proposal used to remember its passage by **character offset**. Accept checked that the text at that offset still matched, then fell back to searching for a unique copy of the text. Two consequences:
- **Wrong copy (#2).** If the author typed text earlier in the document, an *identical* phrase could slide into the old offset. Accept then edited the wrong sentence and reported success.
- **Mid-word insertion (#3).** An insertion has no text to compare, so it was never checked, and it landed mid-word after any earlier edit.

The editor now keeps a **tracked anchor** for the passage, from ⌘K through the streaming reply to the proposal:
- Every insert and delete adjusts its position, including those replayed by undo and redo, via a new `UndoLog.on_op` hook.
- A change inside the passage, or typing exactly at an insertion point, marks it **touched**.
- Accept applies exactly at the anchor, and refuses if the passage was touched or its text differs.
- The text search is gone, and with it the wrong-copy bug.
- The highlight and the insertion marker are drawn from the anchor, so they follow the passage and disappear when it's touched.

The anchor stays touched even if the author undoes the change. That's conservative: Accept refuses rather than guess.

## Findings

Severity: **H** = author text changed without approval, lost, or put in the wrong place; **M** = misleading or broken; **L** = polish.

| # | Sev | Finding | Source | Fix | Verified by |
|---|-----|---------|--------|-----|-------------|
| 1 | H | **⌘↩ accepted the proposal and also typed a newline into the document.** Accept ran first and moved focus to the document; the editor's Enter handler ignored ⌘. | reviewer (reproduced) | In the document, ⌘↩ is a command and never types. | `edits.scribo` (exact text after ⌘↩) |
| 2 | H | **Accept could edit a different copy of the passage.** Text typed before the passage slid an identical phrase into the old offset, which passed the stale check. | reviewer (reproduced) | Tracked anchor (above). | `edits.scribo` ("Then. Hello. The cat sat. THE dog ran."); `test_editor: anchor` |
| 3 | H | **Insertions were never checked for staleness** and landed at a fixed offset, mid-word after earlier edits. | reviewer (reproduced) | Tracked anchor; typing at the insertion point invalidates it. | `edits.scribo` (insertion follows; typing at the point is refused); `test_editor: anchor` |
| 4 | H | **A failed "before" revision didn't stop the edit,** and "Edit applied" overwrote the error. A restore had the same flaw. | reviewer (reproduced) | Accept and restore are refused if the current text can't be written to History first. If only the "after" revision fails, the status says so. | `edits.scribo` (read-only revisions folder: nothing applied, no snapshot) |
| 5 | H | **A reply stopped part-way became a full proposal,** offering to replace the whole passage with its first half. | reviewer | Messages carry a `stopped` flag. A stopped edit reply proposes nothing ("Edit stopped"). Applying a stopped chat reply shows the incomplete-reply warning, and the chat labels it "Stopped before the end". | `edits.scribo` (Stop mid-edit) |
| 6 | H | **An empty or whitespace-only reply would propose deleting the passage.** | author | `llm_clean_edit` returns "" and `propose` refuses it ("returned no text"). | `test_llm: clean_edit`; the `propose` guard by code review (the fake model never replies blank) |
| 7 | H | **⌘↩ in the chat box sent the message and also accepted the open proposal.** | author | ⌘↩ accepts only when the chat box isn't focused. | `edits.scribo` |
| 8 | H | **A selection starting at a blank line was merged into the previous paragraph.** The model drops leading `\n\n`, and the diff showed nothing. | author (probed live) | The cleaner restores the passage's exact leading and trailing whitespace. | `test_llm: clean_edit` |
| 9 | M | **A long passage came back 95% shorter, cut mid-sentence,** and looked like a normal proposal. | author (probed live) | `max_tokens` is at least twice the passage. Passages too long for one edit are refused up front ("too long to edit in one go"). The review warns when a proposal is under 50% of the passage's words, or the reply was cut off. | `test_llm: edit_too_long`; `test_editor: review_warnings` |
| 10 | M | **A question in edit mode came back as the passage plus commentary.** | author (probed live) | The review warns when a proposal is over 250% of the passage's words. | `test_editor: review_warnings` |
| 11 | M | **"Apply to document" on an edit reply with nothing selected inserted the whole rewrite,** duplicating the passage. The code comment also claimed it "re-opens its review", which was never implemented. | reviewer | Applying an edit reply needs a selection ("Select the passage this rewrite replaces"). The comment is corrected. | `edits.scribo` |
| 12 | M | **The cleaner stripped real text.** The preamble rule matched prefixes, so "Surely he knew:" and "Hereafter:" lost their first line. Quote stripping turned `"Run," she said. "Now."` into broken dialogue. | reviewer | Preamble words must be whole words; the line must be short and end in `:`; and it can't be the start of the passage itself. Wrapping quotes are removed only when no quote mark occurs inside. | `test_llm: clean_edit` (7 new cases) |
| 13 | M | **⌘K or Apply while an edit was streaming retargeted it.** The new selection replaced the pending edit's passage. | author (during the fix) | Both are refused until the edit finishes or is stopped. ⌘K closes any open proposal first. | code review |
| 14 | L | **History showed only the 16 newest revisions,** and the Explorer couldn't scroll. | reviewer | Up to 100 are listed, with a note that older ones are in `.scribodo/revisions`. The Explorer scrolls. | code review |
| 15 | L | **Changed line breaks were invisible in the diff.** | author | A pilcrow (¶) marks inserted or deleted newlines. | screenshot |
| 16 | L | **An insertion's target wasn't shown,** and "⚠" rendered as a missing-glyph box. | author | An amber insertion marker, "insert at line N" in the header, and the Miscellaneous Symbols and Dingbats blocks loaded. | screenshot |
| 17 | L | **The model sometimes replaces curly quotes with straight ones** (`“You’re late,”` → `"You're late,"`), cluttering the diff and mixing styles in the manuscript. | author (probed live) | If the passage uses typographic quotes and no straight ones, the rewrite's straight quotes are curled by context: “ or ‘ at the start of a word or after a dash or bracket, ” or ’ otherwise (so apostrophes become ’). Passages with straight or mixed quotes are left as the model wrote them. | `test_llm: curl_quotes` (8 cases) |

The reviewer confirmed that the diff, the revision store and the document-switch handling are sound.

## Verification

- **`make test`:** 4 suites, 9,913 checks, clean under ASan/UBSan (`make sanitize`). New: `test_editor: anchor`, `review_warnings`, and the new `clean_edit` cases.
- **`make uitest`:** 13 scripts, fake model. `edits.scribo` grew from 29 to 53 checks and covers #1–#5, #7 and #11 exactly as the reviewer reproduced them.
- **`make uitest-real`:** `real-chat` (13) and `real-edit` (10) pass with LFM3.
- **Mutation check:** 16 mutations, each validated (the build succeeded and the binary changed). The quote fix (#17, added after) had 4 more: 3 caught; the fourth showed a separate apostrophe rule was redundant (the closing rule already gives ’), so it was removed.
  - **Caught (15):**
    - ⌘↩ typing;
    - the anchor not shifting on inserts or deletes before it;
    - an edit inside the passage going undetected;
    - undo/redo not reported to the anchor;
    - the hook not installed;
    - a touched anchor still used (caught by both UI and unit tests);
    - the "before" failure ignored;
    - a stopped reply proposed;
    - an edit reply inserted without a selection;
    - the preamble prefix match;
    - the preamble-is-the-passage check;
    - dialogue quotes stripped;
    - the short-proposal warning removed.
  - **Not caught (1):** removing the empty-reply guard in `propose`. The fake model can't produce a blank reply, so this guard is covered only by `clean_edit` and code review.
  - **Absorbed:** forcing Accept to use a stale offset was refused anyway by the second check (the text at the anchor must match the original). That mutation was replaced with the stronger "touched anchor still used", which was caught.

## Remaining risks

- **Touched means refused.** Any change inside the passage, even a fix that's undone later, cancels the proposal. The author has to select and ask again. Re-diffing against the changed passage would be friendlier, but harder to make safe.
- **One anchor at a time.** There's one open proposal or pending edit per document. A queue of edits would need a list of anchors.
- **The size warnings are heuristics** (50% and 250% of the passage's words, for passages of at least 12 and 4 words). A deliberate "cut this to one line" gets a warning it doesn't need. The warning is advisory only.
- **Leading apostrophes** ('tis, '90s) in a curly-quoted passage become ‘ rather than ’. They're rare, and the change shows in the diff.
- **Untested by automation:** #13 and #14 (verified by reading the code); #15 and #16 (by screenshot).
