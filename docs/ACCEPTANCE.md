# MVP acceptance (SOW §8)

**Date:** 2026-11-04.
**Machine:** this Mac (Apple M4), macOS 26. By the scope decision of 2026-11-04, the MVP targets this Mac only.
**Build:** `main` at the commit that adds this file.

**Result:** criteria 1–10 pass on this Mac. Criterion 11 passes for macOS; Linux is out of scope. Remaining gaps are listed at the end.

**Update (red-team 009 and the gap fixes):** gaps 1–4 below are closed; gap 5 had its red-team review (REDTEAM-009).

**How it was run:**
- **The full suites:** `make test`, `sanitize`, `tsan`, `uitest`, `dbtest`, `uitest-db`, `backuptest`, `remotetest` and `cstest`.
- **Real-model suites:** `make uitest-real` and `make llmtest`, run with nothing else using the model.
- **The real observer in the full configuration:** project store, semantic index, and the real LFM3 and Qwen3-Embedding models, all under a network watch (criterion 8).

| # | Criterion | Evidence | Result |
|---|-----------|----------|--------|
| 1 | Create a project, brainstorm with a local model offline, close the app, and resume the same chat. | `uitest-db` runs 1→2: a chat in the store, the app restarted, the chat reopened and continued. `real-chat.scribo` uses the real LFM3. `projects.scribo` covers creating a project from the welcome screen. | ✅ |
| 2 | Create or import a Markdown document and work on it in the same session without changing writing modes. | `projects.scribo` and `projects_rt.scribo` cover New and Import in the Explorer and the empty-project screen. `files.scribo` covers saving, conflict copies and read-only files. | ✅ |
| 3 | Ask for an edit, inspect the proposed change, accept or reject it, and restore the previous revision. | `edits.scribo` (fake model: propose, review, accept or reject, restore from History, undo). `real-edit.scribo` uses the real model. | ✅ |
| 4 | See observer findings generated in parallel, each linked to exact source material. | `observer.scribo`, and `real-observer.scribo` against the store: findings with located evidence, and "Source" selects the exact passage. Chat has priority over the observer (MODEL-001 scheduling). | ✅ |
| 5 | Select a finding and explicitly send it to the creative chat without unrelated findings being injected. | `observer.scribo`: Discuss puts that one finding in the message box and the author sends it. `observer_actions.scribo`: nothing is injected into what the author is typing. | ✅ |
| 6 | Save a finding as a note or commit it as canon, then see future potential conflicts flagged against that canon. | `notes.scribo` (note and canon actions). `real-observer.scribo` against the store: canon "Zoë's eyes are green.", then the text "Zoë's brown eyes…". The real observer flagged *"The new text states that Zoë has brown eyes, while canon establishes her eyes are green."* Candidates came from semantic retrieval. | ✅ |
| 7 | Navigate an entity's stored relationships and open their supporting sources. | `5_entities.scribo`: entities and a typed link from findings; "›" goes to the other entity, and the relationship opens its passage. `dbtest: entities` covers normalisation, dismissal, backfill and project isolation. | ✅ |
| 8 | Complete the above without any network connection; optionally choose a configured remote model with a clear data-sharing indication. | **Offline:** during the full real-observer run, every internet socket of the app and both model servers was sampled every 0.5 s. All of them were loopback: MariaDB 127.0.0.1:33061 (164 samples) and Qdrant 127.0.0.1:6333 (1). The model servers use private Unix sockets, and the services bind 127.0.0.1 only. **Remote:** `make remotetest`: off unless configured, selected only in the toolbar, with an amber toolbar indicator and "↗ sent to host" in the chat header. Exactly one request reached the stand-in provider, with its document, and nothing after switching back. | ✅ (see gap 1) |
| 9 | Confirm the active local generative model is LFM3-1.7B with BF16 weights and inference, without quantized checkpoints. | The precision gate in `src/llm/server.c` refuses anything but the pinned BF16 GGUF (sha256 checked once and cached). The server runs with `-fa off -ctk bf16 -ctv bf16`, so it's BF16 end to end ([MODEL-001](MODEL-001-lfm3-runtime.md)). The toolbar shows "LFM3-1.7B · BF16 · local". `make llmtest` covers the supervisor and authentication. | ✅ |
| 10 | Rebuild a wiped Qdrant index from MariaDB and project files; retrieved passages keep exact provenance and canon/draft distinctions. | `dbtest: semantic`: wipe, then rebuild, then the same hits with revision ids and ranges. Retracted canon and notes never count as canon. An index lost while its marks remain is rebuilt on the next sync. `backuptest`: total loss, restore, and the index rebuilt. A real-embedder probe got 7/7 paraphrased questions to the right canon entry, with no leaks. | ✅ |
| 11 | Launch the native raylib + raygui + Clay workbench on macOS and Linux, and validate document editing, resizing and keyboard input. | **macOS:** `smoke`, `typing`, `navigation`, `mouse`, `scroll`, `clipboard` and `panes` (resizing), plus `cstest` (a case-sensitive volume). **Linux:** out of scope for this MVP. | ✅ macOS, ⏸ Linux |

## Gaps and risks

All five are closed.

1. ~~Offline was shown by observation.~~ **`make offlinetest`** runs everything inside a macOS sandbox that denies all network access except this machine: the app, both llama-servers and their children. Controls prove it: an outside host is unreachable inside the sandbox and reachable outside it, while loopback works. The store suite (fake model), real chat, real edit, the real observer (on files and on the store with the real embedder) and the real model's end-to-end suite all pass inside it.
2. ~~Remote messages aren't marked in the saved chat.~~ Each message sent to or answered by a remote model records where ("Frontier · host"), in the chat files and the store (migration 005). It's shown in amber when the chat is reopened. Remote requests carry only what was already exchanged with the remote: earlier local turns stay on this Mac.
3. ~~Real-model checks are loose.~~ The real observer (temperature 0) must flag the conflict against the committed canon entry, quoting the contradicting sentence. Each checked finding's Source must land on the exact passage ("Source passage selected", never "moved since"). Chat checks the name "Zoë". Real-model chat runs use a fixed seed (`SCRIBODO_SEED`) for reproducibility.
4. ~~Backup and restore are command-line only.~~ Explorer → PROJECT has **Back up now** (in the background, with a per-project export keeping original dates) and **Restore from a backup…**, which opens the project as a new folder and never overwrites anything or touches the database (`6_backup.scribo`). Whole-database disasters stay with `tools/restore.sh`.
5. ~~Not red-teamed.~~ [REDTEAM-009](REDTEAM-009-graph-backup-remote.md): 9 findings, including one high (crafted backups), all fixed; 21/21 mutations caught.

Remaining, by design:
- Linux is out of scope.
- Entity names in scripts without capitals (CJK) aren't recognised.
- A first name's folding into a full name depends on the order names arrive in.
