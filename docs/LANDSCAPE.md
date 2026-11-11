# Scribodo: capabilities and gaps

**Date:** 2026-11-11. **Tree:** `5b62793` (AUDIT-003).

One table, one row per area of the product. *Capability* is what works today
and the dated record that measured it. *Gap* is what is missing, unmeasured or
blocked. *Status* uses the project's own words: **Done** (in the product, no
flag), **Experiment** (behind a default-off flag with a measured record),
**Open** (not started or not solved), **Out of scope** (by MVP decision).
Numbers are copied from the records they cite; nothing here is a new
measurement. Update this file when a record changes a row.

## The table

| # | Area | Capability today | Gap | Status | Record |
|---|---|---|---|---|---|
| 1 | Native workbench | raylib + Clay workbench: explorer, tabbed chat and editor, observer pane, resizable panes, UTF-8 editing, undo and redo, clipboard; 398 scripted UI assertions pass; frame worst case 32 ms on a 12,000-word chapter | Real input (IME, dead keys, layouts), VoiceOver and keyboard-only use untested; all layout lives in `main.c`, each new view costs it about 150 lines | Done; GAP-005 open | ACCEPTANCE §11, MEMORY-019, PRODUCT-GAPS, AUDIT-003 F2 |
| 2 | Projects and documents | Create, open, import, rename, edit and save `.md` and `.txt`; every accepted edit is an immutable revision; files stay human-readable in the project folder | DOCX, PDF, layout and publishing | Done | ACCEPTANCE §2, §3 |
| 3 | Creative chat, local model | Persistent per-project chats; streaming replies from LFM3-1.7B in BF16 end to end, pinned by hash and refused otherwise; bounded 64-message window opens a 1,116-turn chat in 0.09 s | A real reply on a long chat takes 84 s on this Mac; the model's faithful use of correct evidence is the unsolved wall (row 12) | Done | MODEL-001, ACCEPTANCE §1, §9, MEMORY-019 |
| 4 | Reviewed AI edits | Proposed edits shown as diffs, accepted or rejected, reversible from History; works with the real model | Edit quality unmeasured beyond one real-model script | Done; GAP-001 open | ACCEPTANCE §3 |
| 5 | Observer | Background extraction of entities, events, relationships, open questions and continuity conflicts, each linked to an exact source; a committed canon entry flagged a contradicting sentence with the real model; chat has priority over the observer | Precision and recall unmeasured on representative novels; names without capitals (CJK) unrecognised; name folding depends on arrival order | Done; GAP-001 open | OBSERVER-001, ACCEPTANCE §4 to §6 |
| 6 | Notes and canon | Findings become notes (editable, non-authoritative) or canon (author-approved, sourced, versioned); nothing becomes canon automatically; canon is project-wide for recall | Canon-aware chat (row 14) does not advance | Done | SOW §4.4, MEMORY-015 §5 |
| 7 | Entities and links | Typed links from findings, navigable to the other entity and its passage; normalisation, dismissal, backfill and project isolation tested | Advanced graph analytics and timelines | Done; analytics out of scope | ACCEPTANCE §7 |
| 8 | Structured store | MariaDB as authority for records, chats, revisions, findings, canon, entities and jobs; 15 embedded migrations; legacy file projects imported on first open in the background; file, database and paused modes behind one ops table; red-teamed | Migration idempotency has no guard (015 lacked `IF NOT EXISTS` until AUDIT-003); crash points at every journal, commit and marker transition untested; two memory workflows bypass the store seam | Done; GAP-004 open | STORE-001, REDTEAM-008, -010, AUDIT-003 F1, F2 |
| 9 | Backup and restore | In-app backup and restore, transactionally consistent export, checksum-verified, regular files only; whole-database disaster recovery by script | Power loss and disk-full at each boundary are simulated nowhere | Done; GAP-004 open | REDTEAM-009, ACCEPTANCE gap 4 |
| 10 | Semantic index | Qdrant as a rebuildable derived index; wipe and rebuild returns identical hits with provenance; retracted canon never counts as canon; pinned Qwen3-Embedding-0.6B | One TCP connection per request at 14 call sites; the reranker's memory figure is a placeholder | Done | ACCEPTANCE §10, TECHNICAL-DEBT |
| 11 | Memory: retention | Every message, revision, note and canon version kept and addressable with exact provenance; delete nothing, only archive; reach@candidates 1.00 on the 30K and 100K fixtures | None known | Done | MEMORY-015 §3, MEMORY-017 |
| 12 | Memory: selection | Production planner admits the answering unit at 0.94 (30K) and 0.98 (100K) on the synthetic fixture; 0.71 on fourteen questions over the first real novel (unchanged at the current tree) and **1.00 on fifteen held-out questions over a second novel** with first-person narration and invented names (2026-11-11); the coreference index layer raised designated units in the eight boundary cases from 4 to 6 of 8 | Implied intent (patterns across turns) unsolved by design; the index layer needs a Python resolver and lives behind a flag with a schema column; the first novel's misses (pronoun-subject attributes) are not yet measured on a second book; single-quote dialogue untested | Done for stated facts; Experiment for the index layer; generalisation measured once | MEMORY-017, -018, -021, journal/answer-cycle3_coref.md, journal/answer-cycle4_mars.md |
| 13 | Memory: faithful use | Verified pipeline (reranker, span extraction, claim gate with number forms, quotation check) behind `SCRIBODO_ANSWER_PIPELINE=verified`; first novel's last run: 1 correct, 0 wrong shown, 3 wrong withheld; held-out novel's first run (2026-11-11): 3 correct shown, 0 wrong shown, 2 correct withheld by a gate false anchor since fixed, 1 selection miss; 0 fabrications shown in any recorded run | The composer imitates a withheld notice on later turns, so withholding costs correct answers; run-to-run variance unbounded; orchestration untested except by live runs; the gate's capitalisation rule was tuned on the held-out novel, so that fixture no longer holds out the gate | Experiment; window construction is the first open item | journal/answer-cycle3_coref.md, journal/answer-cycle4_mars.md, REDTEAM-021 |
| 14 | Canon-aware chat (P1) | Implemented behind a per-session flag; removed every authority and open-state failure in its layout | Introduced over-anchoring in 4 of 27 cases; stop condition met; does not advance in this form | Experiment, stopped | MEMORY-020 |
| 15 | Deterministic recall view | Host-rendered search over committed canon and original excerpts with exact text, source and coverage line; no model on the path; 0.25 s over a 1,116-turn archive | None known; the surface is bounded by what the author stated | Done | MEMORY-016, MEMORY-019 |
| 16 | Offline and remote | Every core workflow proven offline inside a network-denying sandbox; optional OpenAI-compatible remote, off until configured, amber indicator, marked in saved chats, no automatic fallback | None known | Done | ACCEPTANCE §8, gaps 1 and 2 |
| 17 | Scale and sustained use | Legacy import, memory enable (111 s for 1,116 turns with the real tokenizer) and recall run off the frame thread; frames under 25 ms through every phase but chapter save (32 ms) | Workload budgets, soak and restart scenarios, disk growth, observer backlog unmeasured; the one baseline is one project run once | Open; the P0 gap | MEMORY-019, PRODUCT-GAPS GAP-003 |
| 18 | Verification | 17-target matrix: 95,813 C checks and 142 Python tests under plain, ASan/UBSan and TSan builds; 398 + 281 + 35 UI assertions; 24 real-model checks; 110 sandboxed script passes; 10 of 10 mutants killed; analyzer 0 warnings; pins and advisories checked weekly | Real-model acceptance is manual; direct pushes reach `main` before CI reports and verdicts have twice gone unrecorded; a false "dbtest passed" claim stood for three commits | Done; process Open | AUDIT-003, TECHNICAL-DEBT CI section |
| 19 | Distribution | Source build with one Makefile; Docker Compose for MariaDB and Qdrant pinned by digest; model fetch by script | No signed app, no guided setup, no upgrade or uninstall path; never installed on a clean machine | Open | PRODUCT-GAPS GAP-002 |
| 20 | Evaluation corpus | Two public-domain novels: Pride and Prejudice (14 reach, 8 boundary questions) and, held out since 2026-11-11, A Princess of Mars (15 reach, 8 boundary questions, written before any run and never tuned against); synthetic 30K and 100K chat fixtures with planted facts, revisions and retractions | No author-written questions; no modern text; the held-out set lacks pronoun-subject attribute questions, the class the first novel exposed; the live pipeline has not yet run on the second novel | Open; first held-out measurement taken | MEMORY-021, journal/answer-cycle4_mars.md, PRODUCT-GAPS GAP-001 |
| 21 | Code health | Register current to 2026-11-11; storage dispatch, worker lifecycle and checked decoders are sound seams; one static global in `main.c` | Nine experiment flags and one schema column with no lifecycle (P1); a 308-line chat builder with no orchestration test; receipts parsed by regex with no contract; `main.c` at 4,555 lines; seven hand-rolled atomic writers; undeclared Python environments | Open | TECHNICAL-DEBT, AUDIT-003 |

## Reading the table

Rows 1 to 11, 15 and 16 are the MVP as accepted on 2026-11-04 plus the
memory retention and recall work since: built, tested, red-teamed, and not
where the risk is.

The landscape ahead has three fronts, in the order the records argue for:

1. **Faithful use (row 13).** Selection is stable across three identical runs
   and the gates now stop wrong statements. What varies is the composer, and
   the mechanism is known: a withheld notice in its window. Window
   construction is software and has a direct measurement waiting (replay the
   last run's order with and without the notice in history). This is the
   first open item in the journal and the cheapest front.
2. **Generalisation (rows 12 and 20).** Every selection fix is general in form
   and was chosen against one book and eight questions. A second manuscript
   of a different shape, with questions written before the pipeline sees them,
   decides whether the next fix is a class fix or another instance. Until it
   runs, no selection number here is a claim about an author's book.
3. **Product readiness (rows 17, 19, 4, 5, 9).** The five gaps in
   PRODUCT-GAPS are unchanged since 2026-11-06 apart from GAP-003's
   foundation. They are work, not research: budgets and soak runs,
   installation, crash points, input and accessibility, and an evaluation
   corpus for the observer.

Row 21 runs underneath all three. The register's P1 entry (experiment flags
without a lifecycle) is the one that compounds: each front above is run as an
experiment, and each experiment so far has left a flag, a column or a tool
behind.
