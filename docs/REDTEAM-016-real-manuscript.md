# REDTEAM-016 — The real-manuscript changes, red-teamed

**2026-11-08. Adversarial review of the five commits of [MEMORY-021](MEMORY-021-real-manuscript.md) (`4ed59b9` to `cdea424`): the saved-file import pass, the native import ids and per-unit rejection, the explorer scroll-into-view for scripted clicks, the UTF-8 text cut and the manuscript tooling.**

**Baseline:** `cdea424`. Every finding below was reproduced against that
baseline before the remediation and re-measured after it; the reproductions
are tests or probe runs that are now part of the repository, not reasoning.

## Findings

### 1. A folder or an unreadable file named like a document blocked the project's move into the store

`import_saved_files` read every `.md`, `.txt` and `.markdown` entry of the
project folder with `file_text_read`, and any status other than OK or
MISSING failed the whole import. A directory named `folder.md`, a file with
a NUL byte, or a file the user cannot read therefore left the project on
its files for good, with the status "a project file couldn't be read or
decoded". Before `e7092e8` such entries were never touched, so this was a
regression introduced by the saved-file pass.

Reproduced: `test_db: import` now puts a `folder.md` directory and a
`binary.txt` with a NUL byte in the fixture. Against the baseline's
`store_import.c` (linked with a stub for the new function) the import fails
at that line with the message above and eleven dependent checks fail
(`build/manuscripts/pride-and-prejudice/runs/redteam016/import-before.log`);
against the remediation the suite passes, 11,276 checks.

Remediation: the pass is now `sdb_saved_file_adopt`, which skips (true, not
adopted) a name that is not a plain document file, a missing file, a
directory, a special file or anything `file_text_read` refuses. Such a
document simply has no saved revision and is not covered by memory; it
never holds the move hostage. The test asserts the import succeeds, the two
entries have no saved identity, and the real documents do.

### 2. The explorer scroll-into-view moved the explorer for toolbar buttons and misreported where they were

`host_element` decided "this is an explorer item" by geometry: an element
whose x lay within the explorer's columns and whose y lay outside the
panel's visible area. The toolbar's Open button sits at x ≈ 88 px, inside
the 210 px explorer, and above it. With the explorer scrolled (a project
with sixty-one chapters, the chat list reached by scrolling), looking the
button up scrolled the explorer by 44 px and reported the button 44 px
lower, where the click landed on whichever chapter the scrolled list had
there. The Save button and the right-hand toggles happened to lie outside
the explorer's columns in this layout, which is why the MEMORY-021 baseline
did not catch it.

Reproduced: `tests/ui/store/19_explorer_scroll.scribo` on the 61 real
chapters opens chapter one (4,510 code points; no other chapter is within
4,500..4,520), clicks the off-screen chat item, then clicks Open with the
folder dialog pre-answered Cancel. Against the baseline app the editor
afterwards holds a text of 11,813 code points, chapter 36 (`build/manuscripts/pride-and-prejudice/runs/redteam016/probe-before.log`,
run with the baseline binary built from `cdea424` and the same script
without the new assertion). Against the fixed app chapter one is still open
and the button's reported centre stays within the top 40 px before and
after the scroll (`probe-after.log`, 10 of 10).

Remediation: `clay_element_within(ancestor, id)` in `vendor_impl.c` walks
the last layout's element tree from the Explorer; `host_element` scrolls
only for the Explorer's own descendants. `expect element_y NAME MIN MAX`
was added to the script language so a test can assert where a click would
land. The `tests/ui/store/16` baseline and the full UI suite are unchanged.

### 3. A project moved into the store before `e7092e8` has no saved revisions, so memory still cannot cover it

The saved-file pass runs inside the import, and the import runs once per
project folder. Every project already in the store keeps `saved_revision_id`
NULL for every document until the author opens and saves each one in the
app. Measured on this Mac's production database (read-only): one project
(`/Users/ztflynn/Documents/AEON/scribodo`), one document, one revision, and
the `documents` table does not yet have the `saved_revision_id` column at
all, so the first launch after the schema migration will put every existing
document in exactly this state.

Remediation: the memory worker adopts lazily. When an attached document's
saved identity is missing, `memory_documents_capture` and the cached
lookup call `sdb_saved_file_adopt(..., import=false)`, which in its own
transaction gives the document a revision holding its on-disk text
(reusing a revision with that content, else an "import" revision) and sets
the saved pointer only if none exists and the current pointer only if none
exists, so an in-app save receipt or a newer recorded revision is never
overridden. Dirty documents never reach this path, and the existing
hash-against-disk verification runs afterwards as before. Measured:
`test_memory_document_capture` (adoption offered once, taken once, a dirty
document never offered), `test_db: import` (pointer removed, adopted back
onto the revision that already holds the text, no duplicate, second call a
no-op; folder, NUL file, missing file and a `../` name refused as
specified; import mode refused outside a transaction).

Measured end to end in the application (fake model, disposable database,
`build/manuscripts/pride-and-prejudice/runs/redteam016/adopt-*`): a first
session moved the eight-part project into the store (8 documents, 8 saved
pointers, 8 revisions); SQL then removed every saved pointer, the state of a
project moved before `e7092e8`; a second session attached the eight parts
and enabled memory. `wait_memory_documents 8` was met, all eight pointers
were back on the existing import revisions (still 8 revisions, no
duplicates) and 2,070 document units were ingested.

### 4. Hardening without a reproduced failure

- **Import revision ids** were derived from the project, the file name and
  the file's modification time. Two different contents with the same name
  and the same mtime second would share an id, and `INSERT IGNORE` would
  silently keep the first while the saved pointer moved to it, after which
  the disk-hash check would refuse coverage with no message. This needs a
  second import of the same project id, which the import's one-shot
  transaction does not allow today, so it was not reproduced. The id now
  also hashes the content's SHA-256, so the same text gets the same id and
  different text never shares one.
- **`text_cut` returning 0.** A window with no character boundary
  (malformed UTF-8, or a cut inside the first character) returns 0; both
  callers treat 0 as a failed unit, not a loop. Documents reach memory
  through the store's utf8mb4 columns, which reject malformed text, so the
  case is unreachable from a real document; `test_core: text_cut` now pins
  the contract.

### Reviewed and left alone

- The worker's three-strike rule counts consecutive backend failures and
  resets on success; a rejected unit does not count. A rejected unit is
  re-offered at most three times, five seconds apart, then marked failed,
  so it cannot spin the drain.
- `click_element` and `right_click_element` start a new batch before the
  click so the scroll applied by the lookup is laid out first; the UI
  suite's 398 checks are unchanged.
- The import's file fingerprint covers the document files, so a file
  written by another process during the saved-file pass retries the import
  as before. A forked project's pass reads the fork's own folder.
- `import_once` still registers a directory named like a document as a
  document row, as the explorer lists it; it has no content and no saved
  revision. Pre-existing, shared with the explorer and the observer, not
  changed here.

## Verification

| Suite | Result |
|---|---|
| `make test` (26 suites) | all pass (`build/make-test-redteam016.log`) |
| `make uitest` (23 scripts) | 398 checks pass (`build/make-uitest-redteam016.log`) |
| `make dbtest` (12 suites) | all pass (`build/make-dbtest-redteam016.log`) |
| `make tsan` | clean (`build/make-tsan-redteam016.log`) |
| In-app adoption, two sessions on one database | 8 of 8 parts adopted and captured (`runs/redteam016/adopt-*`) |
| `tests/ui/store/19_explorer_scroll.scribo` on the 61 chapters | fixed app 10/10; baseline app 5/6, chapter 36 opened instead of the dialog |

## Limits

- Lazy adoption covers documents that have a row and a readable file. A
  file the app cannot read as text stays uncovered, silently; the status
  line does not say which attached document was skipped.
- A document edited and saved outside the app after its saved revision was
  recorded is not covered until it is saved in the app again: the disk hash
  no longer matches the receipt. That is the existing contract, unchanged
  here, and a product question rather than a defect.
- The explorer ancestry check walks the last laid-out tree to a depth of 64.
