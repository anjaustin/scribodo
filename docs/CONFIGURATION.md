# Configuration

Every setting has a working default; nothing here needs to be set to use Scribodo.
These variables override where Scribodo finds its services, models and configuration
files, and select the experiments kept in the tree for repeat measurement.
`tools/experiment_arms.py` pins every experiment arm a measurement run leaves unset to a stated value, and records diagnostics and test hooks when set
([TESTING](TESTING.md#test-harness-environment-variables)).

## App, models and files

Where Scribodo finds its models and configuration files.

| Variable | Default | Effect |
|---|---|---|
| `SCRIBODO_STORE` | on | `off` runs without the MariaDB project store, using the `.scribodo/` file backend only. |
| `SCRIBODO_LLAMA_SERVER` | `llama/bin/llama-server` beside the app binary | The pinned `llama-server` executable. |
| `SCRIBODO_MODEL` | a Hugging Face cache copy, else `~/Library/Application Support/Scribodo/models/LFM3-1.7B-BF16.gguf` | Generative model file. The pinned BF16 hash is still enforced. |
| `SCRIBODO_EMBED_MODEL` | `~/Library/Application Support/Scribodo/models/Qwen3-Embedding-0.6B-f16.gguf` | Embedding model file. The pinned hash is still enforced. |
| `SCRIBODO_RERANK_MODEL` | `~/Library/Application Support/Scribodo/models/Qwen3-Reranker-0.6B-bf16.gguf` | Reranker model file for the verified-answer pipeline. The pinned hash is still enforced ([MODEL-002](MODEL-002-qwen3-reranker.md)). |
| `SCRIBODO_MEM_AVAILABLE_MB` | measured from the OS | Overrides available memory when sizing the model context window. |
| `SCRIBODO_REMOTE_CONFIG` | `~/Library/Application Support/Scribodo/remote.json` | Optional remote provider configuration. |
| `SCRIBODO_MODEL_SETTINGS` | `~/Library/Application Support/Scribodo/model-settings.json` | Sampling and context settings saved from the toolbar. |
| `SCRIBODO_PROFILE` | `~/Library/Application Support/Scribodo/profile.json` | Author profile. |
| `SCRIBODO_APPEARANCE` | `~/Library/Application Support/Scribodo/appearance.json` | The fonts chosen in the **Fonts** panel: a face and size per role ([USER-GUIDE](USER-GUIDE.md#fonts)). |
| `SCRIBODO_BACKUPS` | `~/Library/Application Support/Scribodo/backups` | Where **Back up now** writes. |
| `SCRIBODO_PROSE_FONT` | unset | A font file tried first for document and chat text, ahead of the Fonts panel's choice, for this run only (never saved). |
| `SCRIBODO_TOOLS_DIR` | the bundle's `Resources/tools`, else `../tools` beside the app | Where the app finds `services.sh` and `backup.sh`. |

## Services

Ports, folders and binaries for the MariaDB and Qdrant services; see [SERVICES](SERVICES.md).

| Variable | Default | Effect |
|---|---|---|
| `SCRIBODO_DB_PORT` | `33061` | MariaDB port on 127.0.0.1. |
| `SCRIBODO_QDRANT_PORT` | `6333` | Qdrant port on 127.0.0.1. |
| `SCRIBODO_SECRETS` | `~/Library/Application Support/Scribodo/services` | Folder holding the `db_password` and `qdrant_api_key` files that `tools/services.sh` generates. |
| `SCRIBODO_SERVICES` | the remembered mode | `native` or `docker` for one `tools/services.sh` call; never remembered. |
| `SCRIBODO_LOGS` | `~/Library/Logs/Scribodo` | Where the native MariaDB and Qdrant write their logs. |
| `SCRIBODO_MARIADB_BIN` | `/opt/homebrew/opt/mariadb@11.4/bin` | Folder with `mariadbd` and its clients. |
| `SCRIBODO_MARIADB_VERSION` | `11.4.13` | The pinned MariaDB release (`tools/services.defaults`). Native mode starts the pinned release silently, a newer 11.4 patch with a warning shown once in the app, and refuses another series or an older patch. |
| `SCRIBODO_QDRANT_BIN` | `build/qdrant/qdrant` | The Qdrant executable (`make qdrant`). |
| `SCRIBODO_COMPOSE_PROJECT` | `scribodo` | Docker mode's Compose project; throwaway stores use their own. |
| `SCRIBODO_MARIADB_MEM_LIMIT` / `SCRIBODO_QDRANT_MEM_LIMIT` | `768m` / `1g` | Docker mode's memory caps. |
| `SCRIBODO_NATIVE_FRESH` | unset | `1`: `up` starts an empty native store over credentials whose store is in the containers. |
| `SCRIBODO_SERVICES_LOCK_WAIT` | `300` | Seconds `services.sh` waits for another run's store lock. |
| `SCRIBODO_SERVICES_LOCKED` | set by the script | Internal: marks the re-run under the store's lock. Do not set. |

## Memory and answers

The memory and answer paths. Each experiment's measured record is in the journal named in its row; a concluded experiment stays in the tree behind its flag ([MEMORY-015](MEMORY-015-effective-memory-contract.md) §5).

| Variable | Default | Effect |
|---|---|---|
| `SCRIBODO_MEMORY_EXPERIMENT` | on | Local memory ([USER-GUIDE](USER-GUIDE.md#local-conversation-memory)) is on since 2026-11-14 (`journal/answer-cycle4_default_path.md`); `off` or `0` runs without it. Consent is still given per conversation. |
| `SCRIBODO_ANSWER_PIPELINE` | on | The verified-answer pipeline answers in conversations with memory on, since 2026-11-14: the local Qwen3-Reranker picks one unit from 64 candidates, one extraction call copies the stating sentence, the span is byte-checked against the unit, and only that span reaches the model (0 fabrications shown in any recorded run; 10 to 15 s of reranking before the reply). `off` or `0` selects the plain composer. Needs the pinned reranker GGUF (`SCRIBODO_RERANK_MODEL`); without the file the app says so on stderr and answers plainly. See `docs/MODEL-002-qwen3-reranker.md`. |
| `SCRIBODO_MEMORY_BRIEFS` | unset | `1` additionally enables generated T1 briefs. Requires the experiment flag. |
| `SCRIBODO_MEMORY_PRESENTATION` | `legacy` | `compact` or `episode` selects an experimental source layout for memory requests. Diagnostic only; requires the experiment flag and never changes what is eligible. |
| `SCRIBODO_MEMORY_EXCERPTS` | unset | `1` selects the failed query-window development variant. Reproduction only; off by default. Requires local memory. |
| `SCRIBODO_MEMORY_SMALL_UNITS` | unset | `1` tests 128-token saved-document units and a 16-unit candidate packet. Failed its 14/14 development reach target; off by default. Rebuild memory to change existing units. |
| `SCRIBODO_ANSWER_BLOCK` | unset | With the verified pipeline, `message` places the verified block at the top of the author's newest message (the pass 3a shape); the default places it in the system message after the composer instruction (pass 3b, measured as the better shape in journal `answer-cycle2_pipeline_live.md`). Kept for repeat measurement. |
| `SCRIBODO_ANSWER_PARTS` | unset | With the verified pipeline, `1` splits a multi-part question into its parts (each with the request's context sentence), retrieves and reranks one candidate pool per part and assigns the two verified spans as a maximum-score matching. Measured live and rejected (journal `answer-cycle3_relation.md`, pass 7): a part with no manuscript answer still gets a slot. Kept for repeat measurement; the default is one pool for the whole request. |

## Observer

Observer experiments, off by default. The app shows the measured conflict figure for whichever combination is set (`src/llm/observer_arms.c`).

| Variable | Default | Effect |
|---|---|---|
| `SCRIBODO_OBSERVER_CONFLICT_GATE` | unset | `1` turns on the observer's conflict-gate experiment: shared titles do not pair a statement with a canon entry (since 2026-11-15 this applies only where the observer picks canon entries to judge, not to note linking), and a conflict verdict must name the two disagreeing values, found in the texts. Measured on both planted novels 2026-11-14 and worse than the default (more off-fault conflicts, two faults lost); kept for repeat measurement, off by default (journal `observer-cycle2_quality.md`, step 5). Any other value runs off with a warning. |
| `SCRIBODO_OBSERVER_SENTENCES` | unset | Pairs a document's own sentences, not the model's statements, with canon. `attributes` (or `1`): a conflict also needs a differing value of one kind (step 8; real model: Mars 6 true of 42 shown, *Pride and Prejudice* 5 of 18). `subject`: it also needs the same subject (step 9; scored offline on recorded replies: 4 of 5, 5 of 7). Off by default; any other value runs off with a warning (journal `observer-cycle2_quality.md`, steps 7 to 9; `journal/audit004-observer.md`). |

## Diagnostics and test hooks

Never set for a measurement unless the measurement is of them. The scripted-suite variables are in [TESTING](TESTING.md#test-harness-environment-variables).

| Variable | Default | Effect |
|---|---|---|
| `SCRIBODO_MEMORY_TRACE` | unset | `1` logs local preparation timings and counts, not prompts or source text. |
| `SCRIBODO_SEM_DEBUG` | unset | Set: failed Qdrant calls are logged to stderr. Diagnostic only. |
| `SCRIBODO_FAKE_EXTRACT_MS` | unset | Test hook: the fake observer backend waits this many milliseconds per extraction. Never set for a measurement. |
