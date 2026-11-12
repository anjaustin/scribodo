# Red-team review 005: observer, notes and canon

**Date:** 2026-11-04
**Scope:**
- notes and canon (commit `3d3eba3`): `records.c`, `record_view.c`, and the panel logic in `main.c`;
- the observer (commit `a7999cf`): `observer.c`, `findings_view.c`, and the sidebar actions;
- the revision snapshots they share.

**Method:** Two passes.
- An **independent reviewer** worked from the code with the fake model, UI scripts and C probes. It reproduced 11 of its 16 findings.
- The **author's pass** ran the real model (LFM3, production flags) with a prompt-injection document and a CRLF document.

Every fix has a regression test. A mutation check, run in a scratch copy with build and binary-change validation, confirmed that breaking each fix fails its test.

**Result:** 16 findings. Five could change canon, notes or findings without approval, or destroy the author's unsent text. All 16 are fixed.

## Findings

Severity: **H** = canon, notes or findings changed, or author text lost, without an explicit action; **M** = wrong provenance, unread text, silent wrong behaviour; **L** = robustness.

| # | Sev | Finding | Fix | Verified by |
|---|-----|---------|-----|-------------|
| 1 | H | **⌘⇧N or a reply's "Note", pressed with an uncommitted canon draft open, turned the draft into a fake stored entry.** `show_record` refused, but its callers carried on: they appended a note and relabelled the canon panel with the note's id. The draft could then be discarded with one Close, or written to `canon.jsonl` under an `n-` id. | `show_record` returns false when the panel isn't free, and every caller stops. `record_view_stored` also refuses an id of the wrong kind. | `notes.scribo` (draft kept, no `n-` line, no orphan note) |
| 2 | H | **Discuss replaced the author's unsent chat message**, and ⌘Z couldn't bring it back. | The finding is *added* after the draft as one undoable edit. | `observer_actions.scribo` (⌘Z restores the draft exactly) |
| 3 | H | **One click that selected a finding could fire its action button.** Selection re-laid out the list on mouse press, and the release landed on a button that had moved under the cursor. Reproduced as a note written and the finding marked "saved". | A button acts only if the press began on that button, on a card that was already selected. | `observer_actions.scribo` |
| 4 | M | **Buttons on a card scrolled out of view still worked**, from the status line above the list. | Buttons must be fully visible, and the press must start inside the list. | `observer_actions.scribo`; mutation of both layers |
| 5 | M | **"Commit to canon" on a finding, with the author's own draft open, linked the finding to that draft.** Committing the unrelated draft marked the finding "committed". | Same as #1. | `observer_actions.scribo` |
| 6 | M | **Text was marked read without being read:**<br>• the part of a paragraph or chat message past 3,000 bytes;<br>• paragraphs past number 4,096;<br>• documents past the first 256 directory entries.<br>The status still said "Up to date". | Paragraphs and messages are read in pieces of at most 2,000 bytes, each tracked on its own. Chat progress is stored as message plus byte offset. The paragraph and directory caps are gone. | `test_llm: observer_redteam` (Zelda, Quentin, Yorick, Ulrika) |
| 7 | M | **Conflict checks were skipped:**<br>• after 6 checks per chunk;<br>• for any statement that duplicated an existing finding. | Every non-question statement is checked, duplicates included, with a budget of 16 per chunk (8 findings × 2 canon entries, so none are skipped). | `observer_redteam` (a repeated "Mara's coat is blue." flagged against new canon) |
| 8 | M | **CRLF and BOM documents had shifted source ranges.** "Source" then selected the wrong copy of a repeated name. Confirmed live: "Dvořák" was stored at 36 instead of 34. | Documents are read as `doc_io` loads them: BOM dropped, CRLF and lone CR turned into LF. | `observer_redteam` (exact range in a BOM+CRLF file); real-model probe |
| 9 | M | **Evidence could be trivial.** Any 2 bytes matched ("ed" inside "climbed"), and a made-up claim could borrow a sentence's first 4 words. **Confirmed live:** a paragraph of planted instructions made LFM3 report "Ingrid is the murderer" with evidence `"the"`, and it was shown in the sidebar. | Matches must be whole words. A prefix must cover at least 60% of the quote. Evidence must be the entity's own name, or a phrase of at least 3 words and 12 bytes. | `observer_locate` (4 new cases); `observer_redteam` (`[[weak]]` hook); real-model probe (only the genuine finding remains) |
| 10 | L | **An unwritable `state.json` caused an endless retry loop.** Every save reset the 5 s timer, and each retry asked the model again. `state.json` was also created with mode 0600. | A write failure backs off for 60 s (saves don't cut it short), with a persistent status. State is written 0644. | `observer_redteam` (attempt counter unchanged across 5 pokes; mode 0644) |
| 11 | L | **The observer could put an older text on top of History**, and the UI and worker recorded revisions without a lock. | Revision recording is serialized. A source snapshot reuses *any* revision with the same text. | `test_llm: records` (an older text reused, History order unchanged) |
| 12 | L | **Listing records was quadratic** (0.64 s for 20k findings, on the UI thread), and every append re-read the log. | Hash index on ids. New records are written without re-reading. 20k findings now list in 0.017 s. | benchmark |
| 13 | L | **Clay reported an out-of-bounds error.** Every Explorer row was a clip (scroll) container, and the notes and canon rows exceeded Clay's limit. | Rows are clipped by the Explorer, not individually. | no Clay errors in any UI test's stderr |
| 14 | L | **Conflicts against canon that was later revised or retracted** stayed "new" and pinned first, and a revised entry was never re-checked. | Such conflicts are shown as "outdated (canon changed)" and not pinned. Dedup compares the canon text, so a revised entry is checked afresh. | `observer.scribo` (retract, then "outdated") |
| 15 | L | **State housekeeping:**<br>• hashes of deleted paragraphs, documents and chats were never pruned;<br>• AI rewrites in the chat log (which may have been rejected) were read as story facts. | State is pruned on every scan. Assistant edit replies are skipped. | `observer_redteam` (pruned state; no "Rasmus" finding from a rewrite) |
| 16 | L | **Polish:**<br>• a chat source switched chats but didn't show the message;<br>• ⌘S in a canon panel was silent;<br>• ⌘⇧N worked from the chat box. | The chat scrolls to the message and outlines it briefly. ⌘S explains that canon is kept with Commit or Revise. ⌘⇧N only works from the document. | `notes.scribo` (⌘S status); chat reveal by screenshot |
| 17 | M | **Two findings created in the same second could share an id** (added 2026-11-12, found by the headless replay in `journal/observer-cycle1_replay.md`). `records_new_id` drew a 16-bit random suffix per call and #12's write-without-reading never checked it; the later finding became version 2 of the earlier record and hid it. Two of six replay runs over a novel lost a finding this way. | The suffix is unique within the process for each second (a random start, then a count, under a mutex); the format is unchanged. | `test_llm: records` (5,000 ids in one loop, all distinct); three replay runs byte-identical on three novels |

## What a prompt-injected document can still do

Evidence must now be the subject's name or a real phrase from the text. A planted paragraph can still get a finding recorded whose evidence *is the planted paragraph*, because the planted text is part of the document. The author sees that quote under the finding. Nothing reaches the chat, notes or canon without the author's click, and Discuss shows exactly what would be sent. The judge sees document text as data, never as instructions to the app.

## Verification

- **`make test`:** 4 suites, 9,998 checks. `make sanitize` and `make tsan` are clean.
- **`make uitest`:** 16 scripts, including the new `observer_actions.scribo` (12 checks). `notes.scribo` grew from 45 to 53 checks and `observer.scribo` from 30 to 31.
- **`make uitest-real`:** chat, edit and observer pass with LFM3. Both real-model probes (injection, CRLF) behave as described above.
- **Mutation check:** 20 of 20 caught. The first pass caught 16; the 4 misses were test gaps:
  - **Orphan note:** the new kind check protected the panel, but a note was still written. The test now checks for that note.
  - **Hidden buttons:** two layers guard this; removing both is caught.
  - **Duplicates not canon-checked:** the test's contradiction wasn't actually a duplicate. Rewritten.
  - **No I/O backoff:** failed jobs never counted as done, so retries were invisible. The observer now counts attempts.

## Remaining risks

- **A paragraph that changes is read again whole.** It's tracked by hash, so findings it already produced are deduplicated, but a reworded event can appear twice.
- **Re-checking a revised canon entry happens only when matching text is read again.** Old conflicts are marked outdated, but text that was already read isn't re-judged against the new wording until it changes.
- **Large projects:** each scan reads every document. That's fine for a novel, but slow for thousands of files. A change-time cache would fix it.
- **Not automated:** the chat-source outline (checked by screenshot), and a project switch during a job (the generation check, verified by reading the code).
