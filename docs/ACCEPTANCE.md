# MVP acceptance (SOW §8)

**Date:** 2026-11-04.
**Machine:** this Mac (Apple M4), macOS 26. By the scope decision of 2026-11-04, the MVP targets this Mac only.
**Build:** `main` at the commit that adds this file.

**Result:** criteria 1–10 pass on this Mac. Criterion 11 passes for macOS; Linux is out of scope. Remaining gaps are listed at the end.

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

1. **Offline was shown by observation, not by a run with networking switched off.** Every socket was sampled and all were loopback. A short-lived connection between samples could be missed, but the HTTP client can only name 127.0.0.1 or a Unix socket. Only the remote adapter, which is off unless configured, can reach another host.
2. **Messages sent to a remote model aren't marked as such in the saved chat.** The indicator shows while it's selected; afterwards, the chat log doesn't say which messages left the device.
3. **Real-model checks are loose,** because model output varies. They check the kind of finding and that it has a source, not exact wording.
4. **Backup and restore are command-line tools** (`tools/backup.sh`, `tools/restore.sh`) without an in-app action.
5. **The entity graph, backup/recovery and the remote adapter haven't had a red-team review yet.** The editor, chat, edits, observer, projects, attachments and the store have (REDTEAM-001 to 008).
