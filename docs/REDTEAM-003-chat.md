# Red-team review 003: chat with the local model

**Date:** 2026-11-04
**Scope:** the chat feature:
- the `llama-server` supervisor (`src/llm/server.c`)
- the Unix-socket HTTP/SSE client (`src/llm/http.c`)
- the chat session, request building and the text rules (`src/llm/chat.c`, `llm_text.c`)
- chat persistence (`src/app/chat_store.c`)
- the chat view, input routing and project wiring (`src/ui/chat_view.c`, `src/ui/input.c`, `src/main.c`)

**Method:** Two passes. In the author's pass, experiments ran against `llama-server` launched with the production flags. An independent reviewer worked from the code alone, using the fake model, stub programs and UI scripts; it never started the real model. Every fix has a regression test, and a mutation check confirmed that reverting each fix fails its test.
**Result:** 19 findings. Five could lose, misplace or silently alter the author's text. All are fixed except two, which are documented at the end.

## Findings

Severity: **H** = author text lost, misplaced or silently altered; **M** = broken feature or a hang; **L** = robustness.

| # | Sev | Finding | Source | Fix | Verified by |
|---|-----|---------|--------|-----|-------------|
| 1 | H | **A chat typed before any document was open was deleted when one was opened.** A project change cleared it, and with no project nothing had been saved. | reviewer (reproduced) | The conversation is **adopted**: bound to the new project, with every message written to its `.scribodo/chats/`. | `chat_projects.scribo` |
| 2 | H | **Switching projects while a reply streamed saved the reply into the other project.** The save path was read when the reply finished, splitting the conversation and leaking book A's text into book B. | both (reproduced) | Each chat is bound to the project it started in, and the switch takes effect only once the reply has finished. | `chat_projects.scribo` |
| 3 | H | **A stream cut off mid-reply was accepted as a complete, successful reply.** It was shown with a Copy link, saved without an error flag, and sent back as context. | reviewer (reproduced) | A stream that ends without `finish_reason` or `[DONE]` is an error ("connection to the model was lost mid-reply"). The partial text stays visible but is never used as context. | `test_llm: truncated_stream_is_an_error` (stub server) |
| 4 | H | **Reasoning leaked into the visible reply and the history.** With thinking off, LFM3 sometimes writes its reasoning straight into the content, closed by an *orphan* `</think>` (1 of 15 prompts). The old rule only recognized an opening `<think>`. It also hid the rest of any real answer that mentioned `<think>` mid-text. | author (probed live) | A leading `<think>…</think>` block is reasoning, and so is everything before an orphan `</think>`. Any other `<think>` is shown as written. | `test_llm: split_thinking` |
| 5 | H | **A torn chat-file line silently swallowed the next message,** including messages from later sessions. The JSON and its newline were written separately, and appends never checked for a trailing newline. | reviewer (reproduced) | One `write()` per line. Before appending, a missing trailing newline is added. Loading recovers records glued onto a torn line by earlier versions. | `test_llm: torn_line_recovery` (checks both the file and the loaded result) |
| 6 | M | **Context budget overflow.** A 3-bytes-per-token estimate let dense text overflow the slot: measured Chinese 2.68, emoji 2.07, numbers/code 1.84 bytes/token. With Include document on, every message in such a chat then failed with HTTP 400. | both (probed live) | The document and history are measured with the server's `/tokenize` (on the worker thread), and the document is trimmed by measurement. The fallback estimate is 1.5 bytes/token. The author is told when the document was cut off. | `request_builder` (dense counter); live: 156 KB Chinese document answered |
| 7 | M | **The budget assumed 32,768 tokens per slot.** The real per-slot context comes from the server's `-c` divided by `-np`, so any other setting overflowed *despite* trimming. Found by the live test above. | author (live) | The budget is read from `llm_server_slot_ctx()`. | `make llmtest` (8K slots) |
| 8 | M | **Stop and quit could hang the UI forever.** `connect`/`send` had no timeout or cancel check, and the UI thread joins the worker. | reviewer (reproduced) | Non-blocking connect with `poll`, a 250 ms send timeout, and checks for cancel and a deadline throughout. | `test_llm: stop_never_hangs` (0.20 s; the reverted fix hangs and is killed by the watchdog) |
| 9 | M | **A second window killed the first window's model server.** It treated the server as an orphan and rewrote the shared socket and key. The two then fought, until both gave up. | both | An `flock` instance lock in the run folder. A second window shows "Another Scribodo window is using the model". Only the lock owner cleans the run folder. | `test_llm: instance_lock` |
| 10 | M | **Orphan kill could hit an unrelated `llama-server`** that reused a stale process ID (it only checked the process name). | reviewer | The process must name *this* run folder's socket in its command line. Holding the lock first means the previous owner really is gone. | code review |
| 11 | M | **The restart budget covered the whole session,** so five crashes spread over a day disabled the model until relaunch. | reviewer | The budget resets after 60 s of healthy running. | code review (needs a 60 s run) |
| 12 | M | **A server that never became ready was left running** (about 4 GB) after the 180 s timeout. | reviewer | It's killed on timeout. | code review |
| 13 | L | **Explorer clicks could open the wrong chat** if the list re-sorted, after a message was saved, between drawing and handling the click. | reviewer | List refreshes are deferred until after click handling, so a click is always hit-tested against the list as drawn. | code review |
| 14 | L | **Stopping before the first token saved an empty assistant turn** and sent it back as context. | reviewer | Such a reply is marked as failed ("Stopped") and excluded from context. This applies to both the real and fake backends. | `test_llm: fake_backend` |
| 15 | L | **The context-overflow error was raw server text.** | author | It now reads: "This conversation is too long for the model's memory. Start a new chat, or turn off Include document." | `test_llm: ok_stream_and_context_error` |
| 16 | L | **Replies stopped at the length limit looked complete.** | author | `finish_reason: length` marks the reply, and the view labels it "Reply stopped at the length limit". | `sse_parsing` |
| 17 | L | **`Transfer-Encoding: chunked` was matched anywhere in the remaining headers,** so another header containing "chunked" could switch the framing. | reviewer | It's matched within its own header line only. | code review |
| 18 | L | **The response decoder had no size limits.** | reviewer | Lines are capped at 4 MB and whole bodies at 64 MB. | code review |
| 19 | L | **UI scripts could run a command before the first layout existed,** so clicks on elements failed. | author (harness) | Two warm-up frames before the first command. | `chat_projects.scribo` |

## Verification

- **`make test`:** 4 suites, 4,773 checks, with ASan/UBSan clean (`make sanitize`). `test_llm` includes a stub server that can cut a stream short, never read, return a context error, or behave correctly.
- **`make uitest`:** 12 scripts, fake model, including the new `chat_projects.scribo`.
- **`make llmtest`** (real model):
  - supervisor start-up past a stale socket, and the `0700` run folder;
  - 401 without the API key;
  - a streamed reply with no reasoning tags;
  - a 156 KB Chinese document answered after measured trimming;
  - zero control tokens from neutralized injection text;
  - SIGKILL, then automatic restart and a working reply.
- **`make uitest-real`:** the real model through the real UI. It answers about the document and the follow-up, Stop works mid-reply, and the chat is saved.
- **Mutation check** (scratch copy, reverting each fix): 8 of 8 caught.
  - Unit tests caught: the truncated stream, the bounded send (it hung, and the watchdog killed and reported it), the torn-line newline, the instance lock, the orphan-`</think>` rule and the measured budget.
  - `chat_projects.scribo` caught: chat adoption and project binding.
  - The torn-line fix was **missed** on the first pass, because load-side recovery alone satisfied the test. The test now also checks the file on disk.

## Remaining risks

- **Orphan `</think>` trade-off.** A legitimate answer that contains a literal `</think>` (e.g. explaining model output) has the text before it treated as reasoning. That's rare in creative writing. The alternative, showing raw reasoning to the author, happened in 1 of 15 real replies.
- **Untested by automation:** #10, #11, #12 and #13 are verified by reading the code only. #11 needs a 60-second healthy run between crashes.
- **One model per machine.** The instance lock makes a second window run without the model rather than share it. Sharing one server between windows would need a broker.
- **Follow-up 2026-11-13 (#9's lock outlived its owner).** The lock descriptor was inherited by the spawned `llama-server`, so after the owning app was killed with SIGKILL its servers kept the lock and every later window reported "Another Scribodo window" (found by the GAP-003 restart run, `journal/gap003-cycle1_series.md`). The descriptor is now close-on-exec and the server is spawned with `POSIX_SPAWN_CLOEXEC_DEFAULT`; `test_llm: orphan_after_owner_kill` pins it.
- **The verified-hash cache** (`model-verified.txt`) trusts path, size, mtime and inode. Any process able to edit the model file as the same user could also edit the cache. Acceptable on a single-user machine.
- **The token budget reserves 4 tokens per message for role markers** and a fixed 256-token margin. Very long histories of tiny messages could under-count slightly. The server's context error is the backstop, and its message is now readable.
