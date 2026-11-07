# MEMORY-001 — Long-lived memory under bounded resources

**Date:** 2026-11-06

**Status:** Investigation and proposed evaluation plan; not implemented.

**Priority:** [GAP-003 — P0](PRODUCT-GAPS.md#gap-003--whole-manuscript-scale-and-sustained-use-p0).

## Recommendation

Yes, Scribodo can adopt the central ideas behind `billion-context` or build a
native equivalent. It cannot give a finite model literal infinite attention or
guaranteed perfect recall. The useful goal is **durable, addressable project
memory with a bounded active context and bounded host-side work**.

Evaluate a small native context planner first: authoritative original text,
incremental hierarchical briefs, and retrieval of exact source passages. Do not
make the full proxy a production dependency yet. Treat this as an experimentally
testable recommendation, not a decision to replace the runtime or implement an
agent platform.

## Upstream examined

Primary repository: [ranxianglei/billion-context](https://github.com/ranxianglei/billion-context).
Inspected commit: `c3dfbe6b21a1b2fc3642594043bc81fd2ae91247`, on `master`;
its `package.json` declares version `0.1.212`. Sources below are pinned to that
commit. No upstream code, installer or tests were executed.

### What it does

- A proxy/host-adapter layer transforms model requests and executes context
  management operations. The model can request compression, decompression,
  context search and status.
- Its `acp-kernel` is a host-independent TypeScript library. The host passes in
  state/messages and persists the resulting state. The kernel performs no model
  inference or storage I/O: **the model or an external summarizer writes the
  summaries**.
- Older material is represented by hierarchical summary blocks and stable
  message/range references. Retained originals and host-managed restoration are
  what make exact recovery possible; a lossy summary is not a lossless encoding.
- Stable rendered blocks/prefixes help provider caching. Scribodo's local LFM3
  cache behavior must be measured separately; remote cache-hit claims do not
  transfer automatically to its recurrent/sliding-window runtime.

### Evidence that limits the "infinite memory" claim

The upstream README explicitly says compression shrinks **outgoing model
requests**, not the host's conversation file. It reports tens of seconds per
turn around 30k messages for one host and recommends starting a new session when
that host becomes heavy. This is directly relevant to GAP-003: token compression
alone is not a bound on application RAM, parsing, rendering or storage work.

The benchmark report hosted in the repository describes an independently
conducted evaluation of **v0.1.202**, not the inspected package version. It uses
DeepSeek V4.1 Flash, Qwen3-Embedding-8B and Qwen3-Reranker-4B, not Scribodo's pinned
models. It reports benefits for some temporal/multi-turn tasks, but also reports
verbatim-needle exact match of **27% for compression versus 69% for full context**
on the cited MemoryAgentBench task (and 59–66.33% for its lexical/reranked arms).
These are reported results, not reproduced measurements or guarantees for this
application. They argue for preserving exact lookup alongside summaries, not for
abandoning all compression or all retrieval.

### License and integration conditions

Both inspected `LICENSE` files contain MIT text **plus an additional visible
product-attribution term**. Reuse requires retaining notices and clearly stating
use of billion-context/acp-kernel with the specified links on a permitted product
surface or shipped documentation. The `license: MIT` package field is not the
whole license. Confirm compatibility and obligations before vendoring, porting
or distributing code; this is not a legal opinion.

| Approach | Feasibility | Main trade-off |
|---|---|---|
| Full proxy as a sidecar | Plausible experimental integration, not demonstrated here | New Node/runtime/service, wire/tool compatibility, independent state, network/update/security policy and failure modes |
| Host-independent kernel via a thin sidecar | Smaller reuse surface than the full proxy | Still needs a JS runtime/bridge, host persistence and summary generation; does not solve paging by itself |
| Native C planner inspired by the design | **Preferred first evaluation** | Fits existing ownership/runtime, but requires our own implementation, tests and quality measurements |

The package declares Node >=20; its documented container uses Node 24 and its
SQLite engine notes require >=22 for that path. Pin and verify any selected
distribution rather than infer deployability from one version field. The full
project also documents online self-update/announcement behavior; a Scribodo
integration would need an explicitly pinned, offline-compatible configuration.
Do not install a generic HTTPS interception layer for a workbench that already
owns its inference calls.

Scribodo currently uses an authenticated private Unix socket for its local model.
A proxy integration needs a demonstrated compatible adapter/bridge and bounded
failure handling; OpenAI-shaped JSON does not make it configuration-only. The
model-driven tool loop and summary quality also need testing with LFM3-1.7B BF16.
A host-scheduled summarizer is a simpler first experiment than making the small
creative model autonomously manage memory tools.

## What Scribodo already has — and what is missing

- MariaDB already preserves project records, conversations, revisions and source
  references; Qdrant is a derived index. Reuse those authorities rather than
  introduce a second authoritative transcript in a proxy.
- `chat_build_request_ex()` keeps the newest messages that fit and limits attached
  documents to a shared budget (currently up to 60% of its input allowance).
  Older conversation content is stored but not automatically recalled here.
- The chat worker copies eligible conversation contents before selecting the
  bounded model request. The chat UI also keeps per-message text/layout caches
  and loops over the message list. These are **inspection-identified scaling
  candidates**, not measured bottlenecks yet.
- The store chat loader queries conversation messages/relations in sequence
  without a page limit. A bounded prompt does not make that load bounded.
- Existing semantic APIs support source-bearing retrieval, but that is not an
  existing general-purpose creative-chat memory planner. Do not silently route
  observer findings through those APIs into creative context.

Relevant code: `src/llm/chat.c`, `src/ui/chat_view.c`,
`src/app/store_chats.c`, `src/llm/semantic.h`.

## Proposed native memory model

### 1. Keep four layers distinct

1. **Authoritative archive:** original messages, immutable revisions and explicit
   author-approved canon. Preserve exact bytes/ranges and stable project IDs.
2. **Derived briefs:** bounded summaries of source ranges, optionally organized
   into conversation/scene/chapter/topic tiers. Store source IDs, revision/hash,
   coverage, summarizer/model/prompt version, eligibility and validity state.
   A brief is not canon, a replacement transcript or a manuscript edit.
3. **Retrieval:** combine exact/lexical lookup with semantic candidates, filter by
   scope/revision/authority, then fetch bounded original excerpts with provenance.
   Avoid trusting a summary for exact quotations, dates or disputed facts.
4. **Working context:** recent turns, the current request/edit passage, selected
   briefs and exact evidence. Do not insert the whole archive or summary tree.

Refresh important briefs from original sources when needed; do not recursively
summarize summaries forever and assume lost details can be reconstructed.
Store originals even when they leave the active context. Storage still has real
capacity, backup and retention costs; "long-lived" does not mean infinite disk.

### 2. Bound the complete request and the host work

For every actual selected model/context configuration:

```text
instructions + current request + protected edit passage + recent turns
  + selected briefs + exact evidence + any tool framing
  + output reserve + safety margin <= effective context limit
```

Use the real local tokenizer/template accounting and the effective limit chosen
after memory fitting, not the advertised 256K or a fixed 32K assumption. Remote
providers retain their separate configured budget and consent rules. Count the
final rendered request, including any tool/role overhead.

Separately cap message pages, layout caches, retrieved candidates/bytes, selected
briefs, summary-job batch size and any retrieval rounds/deadline. Do not clone or
re-tokenize all historical text on every turn. The model window must be bounded
by tokens; host RAM by working pages/cache bytes; work by bounded incremental
operations. Persistent indexes can grow with the dataset, but must not all be
loaded for each request.

### 3. Preserve authority, freshness and privacy

- Initially use the current conversation and explicitly attached sources. Any
  broader project/canon/notes memory scope requires an explicit product decision
  and visible controls; having a record in the database is not permission to
  send it to every model.
- Findings remain excluded unless explicitly discussed. Do not summarize the
  observer's private work into the creative memory channel.
- Local-only history or a brief derived from it must not enter a remote request
  merely because the author selected a remote provider. Eligibility follows all
  underlying sources, not just the summary's label. Preserve the existing remote
  boundary and do not retrieve across projects.
- Bind background summaries to immutable source versions. If inputs or
  eligibility change while a job runs, refuse stale publication and retry within
  a bounded policy. Edits, retractions, deletion and attachment-scope changes
  require invalidation/re-evaluation, including summary ancestors.
- Material retrieved from prose/chat remains reference data, never new system
  instructions. Summary generation cannot promote speculation to canon.
- Expose what memory was used, which original sources support it, and whether
  coverage is partial. Failed retrieval or missing summaries must not silently
  become "there was no prior information."

### 4. Keep foreground writing responsive

Run incremental brief generation as local low-priority work sharing the existing
runtime scheduling discipline. Chat/edit requests take priority over observer and
memory work. Do not load an additional large model just to make a benchmark pass.
Persist completed derivatives transactionally; retain/retry on failure instead
of deleting the original coverage. Bound/cancel abandoned jobs.

Most context construction should use already available briefs and bounded reads,
not an unbounded synchronous summarization cascade. If work cannot fit, show the
author the reduced scope or offer a planned multi-pass task. A whole-book analysis
needs coverage reporting; a top-k result is not an exhaustive manuscript scan.

## P0 execution plan and decision gates

### Stage A — Baseline before choosing an implementation

Create reproducible datasets with seeded exact facts and chronology/corrections.
Suggested sweeps: 100/1k/10k/100k chat messages, 100k/300k/1M manuscript words,
and increasing document/revision/record counts. Treat the largest combinations
as stress cases, not an assertion they are normal single novels. Record exact
bytes/tokens, hardware, context settings, model hashes and warm/cold cache state.

Measure UI/input latency, open/save/history/rebuild times, request preparation,
time-to-first-token, process and whole-system memory, disk/write growth, observer
and summary backlog, cancellation and shutdown. Separate host preparation,
embedding/retrieval and model prefill/generation time. Include long idle/resume,
restart and resource pressure, then an hours-long sustained-use run.

Agree numeric gates before optimization. Candidate UI goals are p95 input-to-paint
<=50 ms and p99 <=100 ms; these are **proposed, not measured or accepted SLAs**.
Define explicit cache MiB/page limits and foreground/background work budgets.
At a fixed active prompt size, test whether per-turn preparation stays bounded
as total history grows rather than merely observing that requests fit the model.

### Stage B — Compare three bounded-context variants

1. Current recent-window/document-cutting behavior.
2. Recent window plus exact/semantic retrieval, without generated memory briefs.
3. Recent window plus hierarchical briefs plus exact/semantic retrieval.

Use the same pinned local models, effective context/output budgets, workloads and
task set. Count summary generation, retries, query embeddings, indexing, restart
costs and cache invalidation; smaller final prompts alone are not a win.

Evaluate exact quotations and unique names, rare facts from early chapters,
cross-chapter chronology, revised/retracted assertions, uncertain/hypothetical
chat ideas and missing-answer abstention. Include prompt injection in source text,
stale jobs, index loss, store-down behavior, cross-project isolation, remote
switches and attachment-scope changes. These targeted recall checks support P0;
they do not replace the broader GAP-001 quality program.

### Stage C — Decide from results

Adopt a memory variant only if it improves relevant recall over the current
baseline without violating the agreed responsiveness, memory, authority and
privacy gates. Structural/source validation does not prove summary truth;
summary fidelity needs its own evaluation. Keep rollback/rebuild paths for
derivatives and require a second adversarial pass before enabling it by default.

If native implementation cost dominates, evaluate a pinned kernel-sidecar arm
against the same tests and include its runtime/licensing/packaging costs. Do not
claim compatibility or downstream benchmark performance until measured locally.

## Source references

All upstream references below are at the examined commit:

- [README: architecture, tool surface, host-history limitation, runtime/update policy](https://github.com/ranxianglei/billion-context/blob/c3dfbe6b21a1b2fc3642594043bc81fd2ae91247/README.md)
- [Kernel README: API, tiers, retained originals and attribution](https://github.com/ranxianglei/billion-context/blob/c3dfbe6b21a1b2fc3642594043bc81fd2ae91247/kernel/README.md)
- [Kernel design: summary generation is external; host owns state/I/O](https://github.com/ranxianglei/billion-context/blob/c3dfbe6b21a1b2fc3642594043bc81fd2ae91247/kernel/DESIGN.md)
- [Reported 1,500-item benchmark and its exact-retrieval limitations](https://github.com/ranxianglei/billion-context/blob/c3dfbe6b21a1b2fc3642594043bc81fd2ae91247/benchmarks/third-party-1500/README.md)
- [Proxy license](https://github.com/ranxianglei/billion-context/blob/c3dfbe6b21a1b2fc3642594043bc81fd2ae91247/LICENSE) and [kernel license](https://github.com/ranxianglei/billion-context/blob/c3dfbe6b21a1b2fc3642594043bc81fd2ae91247/kernel/LICENSE)
- [Package/runtime metadata](https://github.com/ranxianglei/billion-context/blob/c3dfbe6b21a1b2fc3642594043bc81fd2ae91247/package.json)

Local constraints/evidence: `SOW.md` (observer/author authority),
`MODEL-001-lfm3-runtime.md` (local precision, context, memory and cache behavior),
`PRODUCT-GAPS.md`, `TESTING.md`, and the source locations listed above.

## Source-level research follow-up — 2026-11-06

The full [How billion context works report](../reports/How%20billion%20context%20works.md)
examines the same pinned snapshot and separates kernel, orchestration, retention,
recall, caching and empirical evidence. Important refinements: aging marks blocks
old but does not automatically merge them; exposed `search_context` is lexical,
with summary-only search for disk-only foreign sessions; restoration depends on
retained sources. The hosted benchmark describes itself as independent, but its
outcomes were not independently reproduced here. Native-first and hierarchical
memory remain experiment hypotheses, not demonstrated LFM3 quality or adoption
decisions. No upstream tests or dependencies were run or installed.

## MVP design follow-up

The [MariaDB/Qdrant MVP proposal](MEMORY-002-mvp-design.md) applies LMM to the
native planner, scoped retrieval, source-linked briefs, durable indexing and
bounded host history. It includes phase records and a finite protocol checker;
runtime implementation and real-model acceptance remain pending.
