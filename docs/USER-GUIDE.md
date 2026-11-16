# Using Scribodo

This guide covers what each part of the app does and how to use it. To build
and launch the app, see the [README](../README.md#quick-start). To set up the
project store, see [SERVICES](SERVICES.md). The settings you can override are
listed in [CONFIGURATION](CONFIGURATION.md).

## Appearance

Choose **Scribodo ▸ Appearance…** in the toolbar, or press **⌘,**.

- **Theme:** Dark (the default), Light, or Paper, a warm sepia page. Text in each meets the WCAG contrast levels against the surfaces it is drawn on, selections and highlighted menu items included (`tests/test_theme_contrast.py`); dimmed paragraphs (below) are faint by design.
- **Writing:** *Typewriter scrolling* keeps the line you are writing at the middle of the page; *Dim the other paragraphs* draws every paragraph but the one with the caret faint. Both are off by default.
- **Focus mode** (**⌘\\**, or **Scribodo ▸ Focus mode**) hides the Explorer, the chat and the side pane, leaving the page between the toolbar and the status bar; the same keys bring them back. It lasts for the session.
- Panes glide open and shut, the document's scroll glides, and a new finding fades in. With macOS's **Reduce motion** turned on (System Settings ▸ Accessibility ▸ Display) all of it is immediate. The setting is read when Scribodo opens.

### Fonts

Each part of the app has its own typeface and size:

| Role | Default | Used for |
|---|---|---|
| Interface | Share Tech, 14 | menus, lists, buttons and labels |
| Documents | Share, 18 | the text you write |
| Chat | Share, 15 | messages and replies |
| Figures | Share Tech Mono, 13 | the status bar, counts and positions |

- Click a role's face to choose another. The list holds the fonts in macOS's font folders, `/Library/Fonts` and `~/Library/Fonts` (and one folder below each), collections included, that draw Latin letters with outlines, and macOS's own San Francisco, SF Mono, SF Rounded, SF Compact and New York under those names; fonts macOS downloads on demand are not listed. Type to filter it, use the arrow keys and Enter, or click; **Default** returns to the bundled face, and Esc or **Back** returns to the roles without choosing.
- A font from outside macOS's own folders is first opened by a separate, short-lived process. One that would crash Scribodo is refused, the role keeps its face, and the status bar says so. A saved font that has since gone missing is replaced by the default when Scribodo opens, with the same message.
- **−** and **+** change the size. Each row previews its role in the face and size chosen.
- Changes show at once. **Done** (or Enter) keeps them; **Cancel** (or Esc) puts back what the panel opened with; **Defaults** returns the theme, the writing aids and every font to their defaults.
- Interface and Figures sizes stop at 16 for now, while the toolbar, lists and status bar have fixed heights.
- The Share faces cover Western European letters. A letter a face lacks, such as ř in *Dvořák*, comes from Arial at the same height and on the same baseline. The bundled faces and their licence are listed in `assets/fonts/README.md`.

## Projects and documents

A project is a folder of `.md` and `.txt` files.

- On launch, Scribodo reopens the project and document you last had open. The first time, it shows a welcome screen: **Open folder…**, **New project…**, **Open the sample**, and your recent projects.
- **⌘O** or **Open…** opens another folder. Dropping a folder onto the window works too.
- **+ New document** (⌘N) and **Import…** (copies files in, never overwriting) are in the Explorer.
- Right-click a document to **Rename** it or **Reveal it in Finder**. A rename keeps its History, and notes, canon and findings that quote it still open their source.
- You can also drop a `.md` or `.txt` file onto the window to open it. Press ⌘S to save.

## Chat

- **Chat:** click the chat box (or press ⌘L), type, and press Enter. Shift+Enter adds a new line, and Esc returns to the document.
- **Attached documents:** the chips under the chat show which project documents the assistant sees. A new chat starts with the open document attached; **+ Attach** adds others, and × removes one. Documents are read when you send (the open one including unsaved edits), never copied. Each message remembers what it was sent with, and reopening a chat restores its attachments.
- **Note turns:** the composer's **Note** toggle marks a message as your note rather than a question, so the answer checks do not treat it as one.
- **Rename a chat:** right-click it in the Explorer.
- **Reply actions** (Copy, Apply to document, Note, Canon) show under the latest reply and under any message you point at.
- **Copy** puts a reply's text on the clipboard for pasting into the document.
- **Saving chats:** each message is saved to the local MariaDB project store, and past chats reopen from the Explorer. Before a project is moved into the store, or when running with `SCRIBODO_STORE=off`, the file backend uses `.scribodo/chats/` instead.

## Reviewed AI edits

The assistant never changes your document by itself.

- **Ask for an edit:** select a passage and press ⌘K, then describe the change in the chat ("tighten this", "make Dvořák colder") and press Enter. Esc cancels.
- **Review:** the proposal opens as a word-level diff at the bottom of the document, with deletions struck through in red and insertions in green, and the target passage is highlighted. Nothing changes until you choose.
- **Accept** (or ⌘↩ in the document) applies it as one undoable step and records revisions from before and after. If the "before" revision can't be written, nothing is applied. **Reject** discards it. Saving the file is still ⌘S.
- **Warnings:** the review warns when a proposal is much shorter or longer than the passage, or the reply was cut off. A reply you stop part-way is never proposed.
- **Stale proposals:** the passage is tracked through your edits (and undo/redo). Text you add or remove around it moves it. If you change the passage itself, or type at an insertion point, Accept refuses rather than overwrite your newer text.
- **Apply to document**, under a reply, proposes that reply for the current selection (or inserts it at the cursor), through the same review. A rewrite from ⌘K needs the passage selected.
- **History** in the Explorer lists the document's 100 newest revisions (each save, AI edit and restore) from the project store; the Explorer scrolls. Clicking one restores it, undoably, after first recording the current text. The file backend keeps equivalent revisions under `.scribodo/revisions/`.

## Notes and canon

Notes and canon are kept in the local project store. The file backend uses
`.scribodo/notes.jsonl` and `canon.jsonl`.

- **Save as note:** select a passage and press ⌘⇧N, or click **Note** under a chat reply. It's saved at once with its source; edit it in the panel under the document (⌘S saves, and closing saves too). **+ New note** in the Explorer starts a blank one.
- **Commit to canon:** select the passage that establishes a fact and press ⌘⇧C, click **Canon** under a reply, or press **To canon** on a note. It opens as a draft: state the fact, then press **Commit**. Nothing is canon until you do, and uncommitted text needs a second Close to discard.
- **Revise, Retract, Recommit:** each stores a new version. Every earlier version stays listed under the entry, and clicking one loads its wording.
- **Sources:** each entry records where it came from: the document, an immutable snapshot of its text in History, the range and the quote, or the chat reply. Clicking the source line selects the passage again, even if it has moved.

## Recall

Press the **Recall** button, type a question or a few words, and press Enter.
The panel lists, exactly as stored, every committed canon entry whose current
wording contains your terms, then verified excerpts of your original chat
messages and saved document text, best match first.

- Above the rows it says what was searched: how many canon entries, how many conversations had memory on, off, or still indexing, and how many candidates were rejected because their source had changed.
- No model is involved. Matching is plain text, retracted canon is excluded, and other projects are never searched.
- "Nothing matched" means nothing matched in what was searched, not that the book never says it.
- Click a canon row to open the entry; click an excerpt to select its source passage.
- Memory excerpts need the project store and a conversation whose memory you enabled.

See [MEMORY-016](MEMORY-016-recall-view.md) and the contract in
[MEMORY-015](MEMORY-015-effective-memory-contract.md).

## Local conversation memory

Memory is available on every launch, but it reads nothing into a request until
you turn it on for that conversation: open a stored chat and right-click it to
enable, disable or rebuild its memory. `SCRIBODO_MEMORY_EXPERIMENT=off` runs
without it.

- With memory on, Scribodo retrieves older chat and saved revisions of the attached documents, and shows them read-only as **Sources** with the revision they came from. It never silently sends them to a remote provider.
- Local memory reserves room in the request for recalled sources instead of filling the window with recent history first. Messages left out of a request remain retrievable.
- Unsaved buffers, reviewed edits and remote requests use only what is in the foreground.
- In a conversation with memory on and documents attached, the **verified-answer pipeline** answers: a local reranker picks the passage, the stating sentence is copied out and byte-checked against the manuscript, and only that checked span reaches the model. It adds 10 to 15 seconds of reranking before the reply. See [MODEL-002](MODEL-002-qwen3-reranker.md).
- Generated summaries ("T1 briefs") are a separate opt-in behind `SCRIBODO_MEMORY_BRIEFS=1`. They are generated locally, linked to their sources, and never canon.

**Limits.** Delivering the right source is not the same as answering
correctly. The recorded runs show no fabricated statement passing the
verified pipeline, but the model can still misread a relation inside text it
was given. A relation check withholds a reply that states a kinship or
possession relation no source states; other misreadings, such as a wrong
label or the wrong subject of a pronoun, can still pass. The current figures for
each novel tested are in [LANDSCAPE](LANDSCAPE.md) rows 12 and 13, and the
earlier remediation runs in [MEMORY-022](MEMORY-022-product-remediation.md)
and [REDTEAM-018](REDTEAM-018-memory-contracts.md).

## The observer

Open it with the **Observer** button; the count shows new findings.

- It reads what you've **saved**: documents on ⌘S and chat messages as they finish. It lists characters, places and objects, events, relationships, open questions, and **possible conflicts** with committed canon. Each finding quotes its exact source.
- It never writes into the chat, your documents, notes or canon. It pauses whenever chat is active, so replies stay fast.
- Select a finding to act on it:
  - **Discuss** puts it in the message box for you to edit and send. This is the only way a finding enters the assistant's context.
  - **Save as note** stores it as a note.
  - **Commit to canon** opens a canon draft; nothing is canon until you press Commit.
  - **Dismiss** hides it.
  - **Source** selects the passage.
- When the list holds a possible conflict, the panel says how often one turned out real on the test novels (about 1 in 13 with the default settings), so you can weigh them. Possible conflicts are suggestions.
- Findings are kept in the local project store. The file backend uses `.scribodo/findings.jsonl`. See [OBSERVER-001](OBSERVER-001-design.md).

## How saving protects your text

- Saves are atomic, follow symlinks to the real file, and keep the file's permissions, BOM and line endings.
- If another program changed the file since you opened it, Scribodo leaves that version in place and writes yours to a sibling `… (Scribodo conflict <date> <time>).md`. Saving again within 30 seconds, without further edits, overwrites the disk version.
- Files that are write-protected or contain invalid UTF-8 open read-only.
- While a document has unsaved edits, its whole text is written beside it every ten seconds (`<name>.scribodo-XXXXXX`, the same name a save's temp file has). If the app or the Mac dies before you save, the next open keeps that text beside the document as `<name> (Scribodo recovered <date> <time>).md` and opens it; nothing is overwritten. A clean save removes the draft.
- The project store (chats, notes, canon, History) is a MariaDB database. Its commits survive the app or the database dying, which the [interruption matrix](GAP-004-interruption-matrix.md) tests.
- **Losing power is different.** MariaDB syncs each commit with `fsync`, which on macOS hands the data to the drive without forcing the drive's own cache to disk. Forcing it needs `F_FULLFSYNC`, which MariaDB does not use. Measured on 2026-11-14 on this disk over three runs: 0.24 ms a commit, against 4.0 ms with a full flush and 0.05 ms for a plain `fsync` (`build/observer/redteam023/durability/`; [REDTEAM-023](REDTEAM-023-native-services-and-batch.md)). So a commit made in the last moments before the Mac loses power can be lost, natively or in Docker, where the virtual disk adds its own layer. A MacBook on battery rarely loses power without warning; a desktop Mac on a power strip can. Documents are files on your disk either way.
- **Scribodo ▸ Back up now** exports the whole store to files; **Restore from a backup…** is beside it. A backup is the project files plus a transactionally consistent MariaDB export; it accepts only regular files and directories, refusing symlinks and special files, and is checksum-verified before success is reported.

The reviews behind these behaviours are [REDTEAM-001](REDTEAM-001-editor-spike.md)
and [REDTEAM-022](REDTEAM-022-end-to-end.md).

## An optional remote model

You can add one OpenAI-compatible chat-completions provider. It's off unless
configured and selected.

1. Put the API key in a private file: `chmod 600 ~/.config/scribodo-key`.
2. Write `~/Library/Application Support/Scribodo/remote.json`:
   ```json
   { "name": "Frontier", "base_url": "https://api.example.com/v1",
     "model": "model-name", "api_key_file": "/Users/you/.config/scribodo-key",
     "context_tokens": 32000 }
   ```
3. Click the model in the toolbar and choose it.

How a remote model is kept in check:

- **Selection:** every launch starts local, and only your choice in the toolbar switches.
- **Indicator:** while the remote model is selected, the toolbar (in amber) and the chat header say where chat messages, attached documents and edit passages go.
- **No fallback:** if the remote fails, you get its error. Scribodo never switches between local and remote on its own.
- **The observer** always runs locally.
- **Transport:** HTTPS is required, and redirects aren't followed. The key is only read from a private file and is sent only to the configured host.
