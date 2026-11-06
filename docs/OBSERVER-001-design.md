# Observer 001: design and validation

> Red-teamed in [`REDTEAM-005-observer.md`](REDTEAM-005-observer.md). Since then:
> - evidence must be whole words and substantial;
> - text is read in pieces, normalized like the editor;
> - every statement, duplicates included, is checked against canon.

**Date:** 2026-11-04
**Scope:** the parallel observer (SOW §4.3):
- the worker, extraction, evidence matching, deduplication and canon checks (`src/llm/observer.c`)
- findings storage (`src/app/records.c`, `.scribodo/findings.jsonl`)
- the sidebar (`src/ui/findings_view.c`)
- the author's actions in `src/main.c`

## Design

- **Only stored text is read.** That means saved documents and saved chat messages, never the unsaved buffer. `.scribodo/observer/state.json` records which document paragraphs (by hash) and how many messages of each chat have been read. Work is derived from that state, so a crash or restart loses nothing and re-reads nothing. A save or a finished reply pokes the observer, and opening a project picks up anything unread.
- **One request per chunk.** Up to 3,000 bytes of unread paragraphs or messages go into one request. llama-server's `response_format: json_schema` constrains the reply to a list of at most 8 findings, each with a type, subject, description, evidence, and an optional relation and object.
- **Evidence must be found in the source, or the finding is dropped.** Matching is exact first, then ignores quote style, whitespace, `*`/`_` and the ZWNJ that neutralization inserts, then falls back to the longest prefix of 4 or more words. The finding stores the *source's* text and range, never the model's paraphrase. Document sources point at an immutable revision snapshot (usually the save's own revision), and chat sources point at the message.
- **Cleanup:**
  - Duplicates are dropped: entities by name, relationships by subject, relation and object, and others by normalized description.
  - A relationship must link two named entities.
  - An entity description that never mentions its name gets the name in front.
- **Canon checks:**
  - A word-overlap filter (shared names count 2, other shared content words count 1) picks the two most relevant committed canon entries for each finding, up to 6 checks per chunk.
  - Questions are never checked, because a question asserts nothing.
  - Each check is a constrained judgment run twice, with CANON and NEW in both orders. A conflict is recorded only if both runs agree.
  - The result is one `conflict` finding per canon entry and passage, naming the canon entry and quoting its text.
  - Notes that overlap strongly (score of 3 or more) are linked as related, without a model call.
- **Scheduling:** observer work starts only after chat has been idle for 1.5 s. The app reports chat activity every frame, and an in-flight request is abandoned, then retried later, the moment chat becomes active.
- **Authority:** findings appear only in the sidebar.
  - **Discuss** puts the finding, its quote and any conflicting canon text into the message box. The author sees it, can edit it, and sends it. Nothing reaches the assistant otherwise.
  - **Save as note** stores a note with the finding's sources.
  - **Commit to canon** opens a canon draft. The finding becomes `committed` only when the draft is committed.
  - **Dismiss** hides the finding. Every status change is a new version in the log.

## Validation against LFM3-1.7B (BF16, production flags)

**Extraction** (sample chapter, 750 prompt tokens): 8 plausible findings in 20 s. 5 of the 8 evidence quotes were **not verbatim**: the model straightened curly quotes, dropped Markdown `*`, and joined lines. This is why matching normalizes and falls back to prefixes. Entity descriptions often lacked the name ("A man who is hunched over…"), and some relationships were vague ("the eyes … the sea"). Both are handled by the cleanup rules above.

**Conflict judge, as first designed** (a verdict field, then an explanation): 5 of 8 correct. 2 of 6 real conflicts were missed: eye colour, and "never been there" against "coming back".

**Explanation first** (a `check` field before `verdict` in the schema, so the model explains before it decides): 6 of 6 conflicts were caught. On 8 harder non-conflicts, it raised 2 false alarms:
- an open question, which is now never checked;
- "climbed the lighthouse stairs" against "has never been to the lighthouse before".

**Two votes, both orders, conflict only if both agree** (12 cases): 5 of 5 conflicts caught, 1 of 7 false alarms. The remaining false alarm is the stairs case, which is arguably ambiguous. Each vote takes about 3 s.

**End to end** (`make uitest-real`, `real-observer.scribo`):
- 8 findings from a two-paragraph chapter.
- A saved line, "Zoë's brown eyes narrowed at the sea.", against canon "Zoë's eyes are green.", produced exactly one conflict: "The new text states that Zoë has brown eyes, while canon establishes her eyes are green."

**Cancellation** (llama-server with the production flags; an observer request with a 2,300-token prompt, abandoned after 3 s, then a chat request):

| Case | Chat first token | Largest gap between tokens |
|---|---|---|
| Baseline | 0.11 s | 0.04 s |
| Right after abandoning the observer request | 0.08 s | 0.04 s |
| Observer still decoding alongside (prefill done) | 0.15 s | 0.04 s |

llama-server drops an abandoned request immediately. The multi-second stall measured in MODEL-001 comes from observer *prefill* overlapping chat, which the idle gate prevents.

## Verification

- **`test_llm`:**
  - `observer_locate`: curly-quote, emphasis, line-break and prefix matching; invented evidence refused.
  - `observer_text`: paragraphs, overlap, the fake backend.
  - `observer_run`: an end-to-end run on the fake backend covering exact codepoint ranges into the stored revision, the canon conflict with its related text, nothing re-read on re-poke, only a new paragraph read after an edit, duplicates dropped, no work while chat is active and resumption after, chat-sourced questions, status versions, invented evidence never recorded, and non-UTF-8 documents skipped.
- **`observer.scribo`:** findings shown; never in the chat unless sent; Discuss, Save as note, Commit to canon and Dismiss; a conflict from a saved paragraph, listed first even before newer findings; Source selecting the exact passage; no duplicates on re-save.
- **`real-observer.scribo`:** the real model, as above.
- **`make sanitize`** and **`make tsan`** (new; ThreadSanitizer over the chat worker, observer and supervisor) are clean. TSan found one race, in the *test* stub server's stop flag, which is now atomic.
- **Mutation check:** 10 of 10 caught:
  - invented evidence kept;
  - running while chat is active;
  - no dedup;
  - read state not saved;
  - invalid UTF-8 read;
  - no conflict check;
  - Discuss sending by itself;
  - committed when the draft opens;
  - dismissed findings still listed;
  - conflicts not listed first.
  
  The last was missed at first, because the conflict was also the newest finding. The script now adds a newer finding first.

## Limits

- **The judge only sees the canon entries the word-overlap filter picks.** A contradiction that shares no names or content words with the canon text (e.g. "the old woman" against canon about "Grandmother Signe") won't be checked. Semantic retrieval (embeddings and Qdrant, SOW §5) is the planned fix.
- **Extraction quality is a 1.7B model's.** Types are sometimes loose (an event typed as an entity), and descriptions vary run to run. Deduplication by name and description catches repeats of the same entity but not paraphrased events.
- **The two-vote rule trades recall for precision.** A conflict that only one ordering catches is not shown.
- **Untested by automation:** a project switch while a job runs. Its results are discarded by a generation check, verified by reading the code only.
- **The fake backend is rule-based.** It exercises the shared parsing, matching and storage paths, not the model's judgment. That's what `real-observer.scribo` is for.
