# Observer cycle 2: the real model's findings, measured by type

**Date:** 2026-11-13. **Tree:** `adfd9d7` plus the changes recorded below.
**Direction:** P0 working order item 6 (PRODUCT-GAPS): "GAP-001 beyond
selection. Observer precision and recall by finding type on both novels;
author-rated usefulness on complete sessions." The user's standing rule for
every item: measure, always; guess only as a last resort.

## What exists, and what it cannot say

The observer's yardstick (`journal/observer-cycle1_replay.md`) runs it
headless with the **fake** backend over the three test manuscripts and
compares two builds byte for byte. It proves a worker change moved nothing;
it says nothing about whether the findings are right, because the fake
backend's "findings" are the names and questions it is told to see. GAP-001's
exit evidence asks for precision and recall by finding type on real novels
with the real model, documented error analysis, and thresholds agreed before
any tuning. No run so far has produced the real model's findings over a whole
manuscript in a form that can be scored.

## Step 1, done: the replay takes the real model

`tools/observer_replay.c --real` starts the pinned model the way the app does
(`llm_server_default_config` with the binary's own directory as the app
directory, 16K context over two slots, the run folder, server log and hash
cache under `<project>/.scribodo/replay-run`), waits for READY (or FAILED,
which it reports at once with the supervisor's line), hands the server to
`observer_create`, ticks it while the observer works, and destroys it at the
end. The fake path is untouched: `tools/observer_replay_run.sh realflag
build/observer_replay` reproduced 717, 348 and 948 findings byte-identical
to `split2` on the three novels (`build/observer/realflag-*`).

**Smoke, chapter one of *A Princess of Mars*** (2,616 words, 14,210 bytes;
`build/observer/smokereal-mars1.txt`, `.err`): model ready at slot context
8,192 in about 25 s; 6 jobs; 30 findings in 136 s wall including the model's
load, about 22 s a job (the drain run in `gap003-cycle1_series.md` measured
16.5 to 29.7 s a call, median 26.2). By type: 17 entity, 13 event, 0
relationship, 0 question. What the sample shows before any scoring:

- Entities include places and things as well as people ("Arizona Hills",
  "the water hole", "the cave (large cave)"), the narrator under three
  subjects ("I", "the narrator (I)", "John Carter"), and a parenthetical
  habit in the subject field ("Apaches (Indians)", "Apaches (trailer
  riders)"). Any precision rubric has to say whether a true but duplicated or
  oddly named entity counts, before the first scored run.
- Events are faithful one-sentence summaries with verbatim evidence; one
  description is wrong against its own evidence ("lays Powell to die after a
  failed examination" where the text has him examine the body and find no
  spark of life). That is the kind of error the audit must catch and count.
- No relationships, although the chapter states several ("Captain James K.
  Powell of Richmond", a fellow Confederate officer; the narrator's name and
  alias). Recall of stated relationships is a candidate for the lowest number
  on the first scored run.
- The first chapter yields 30 findings for 2,600 words, about 11.5 per
  thousand; the whole novel (65,966 words) would yield around 750 and *Pride
  and Prejudice* (121,791) around 1,400, if the rate holds. A full run is
  about 170 jobs for *Mars* (one hour) and 310 for *Pride and Prejudice* (two
  hours) at 22 s a job, idle machine, AC power.

## The harness, written before the scored runs

Two measurements, kept separate as the contract keeps reach and faithful use
separate:

1. **Recall by type, planted.** A copy of each novel receives a fixed list of
   planted continuity faults, each a one-sentence edit at a known offset that
   contradicts a fact the text establishes earlier (an eye colour, a family
   tie, a date, a place, an injury, whether something has already happened),
   plus a fixed canon list stating the original facts (`--canon`), so the
   judge path runs over them. The list is written and committed **before**
   the first run and never edited to match a result; the expected outcome per
   fault is a `conflict` finding whose source quote covers the edit. Recall is
   planted faults found over planted faults, by fault type (appearance,
   kinship, timeline, place, event-order) and by novel. The unplanted copy is
   run too, so the planted copy's extra findings can be attributed.
2. **Precision by type, audited.** From the unplanted run, a fixed-seed
   random sample of findings per type (entity, event, relationship, question;
   40 each where that many exist) is judged against the source quote and the
   surrounding paragraph by a rubric with three verdicts: *right* (the
   description is true of the text and the subject is a real thing in it),
   *wrong* (the description or subject is false against the text, as the
   "lays Powell to die" event is), *empty* (true but of no use to an author:
   "Arizona moon", the narrator under a pronoun). Precision is right over
   sampled, with empty reported beside it, not folded into either. The rubric
   and the sample seed are committed before the audit; the audit is done by
   hand and recorded line by line in the journal, as the reach harness's
   rubric review was.

The two novels are *A Princess of Mars* (the held-out novel of the selection
work, 2026-11-11) and *Pride and Prejudice*. *Wuthering Heights* stays
unused by this cycle so a third novel remains held out if either of the
first two is ever used to tune the observer. Author-rated usefulness on
complete sessions is the third measurement and needs the author in the
chair; it is not simulated here.

Thresholds are agreed before tuning, as the exit evidence requires: the first
scored runs produce the numbers, the author sets the line, and only then
does any change to the observer's prompts or code get measured against it.
Nothing in this cycle changes the observer's behaviour; the only product
change so far is a measurement flag on a diagnostic tool.

## Gates

On this tree (`build/observer/realflag-gates/gates-*.log`, `gates-exit.txt`):
`make test` 95,994 C checks and 186 Python tests, 0 failures; `make analyze`
0 warnings across 87 source files; `make sanitize` 95,994, 0; `make tsan`
48,624, 0; `make dbtest` 18,000, 0; every exit status 0. Hosted verdicts are
appended when the runs finish.
