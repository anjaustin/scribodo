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

## Step 2, done: the faults, planted and scored by tools

The lists are committed before any scored run: `tests/fixtures/observer_faults/mars.json`
and `pp.json`, ten faults each. Each names a file, an anchor sentence that
occurs exactly once in it, the sentence inserted after the anchor, the canon
statement of the original fact (what the replay seeds with `--canon`), and
where the text states that fact. By type: *Mars* has 2 appearance (the
green Martians' blood-red iris, Dejah Thoris's coal-black hair), 2 kinship
(Dejah Thoris's parentage, Sola's), 3 place (Powell of Richmond, Kantos Kan
of Helium, the Princess of Helium), 2 timeline (March 3, 1866; "ten years
later") and 1 event-order (thirty days to Helium); *Pride and Prejudice* has
1 appearance (Charlotte's age), 5 kinship (Lydia the youngest, Collins the
cousin, five daughters, Gardiner the brother, Lady Catherine the aunt), 1
place (Pemberley in Derbyshire), 2 timeline (Bingley's income, the Bennets'
twenty-three years) and 1 event-order (Mrs. Gardiner's Derbyshire years).
Two pairs share an anchor on purpose (M3 and M4, M6 and M10, P4 and P8, P6
and P9, P7 and P10), so two faults sit in one chunk and the extraction's
"at most 8 findings" cap is exercised.

`tools/observer_plant.py` writes the planted copy and `faults-applied.json`
with each insertion's byte offset and length (written believing the observer's
source spans were bytes; they count characters, found and fixed in step 3
below, the novels having three-byte quotation marks before most anchors);
`tools/observer_score.py` reads the replay's lines and counts a fault found
when a `conflict` finding's span overlaps the inserted sentence, one finding
per fault, and reports recall by fault type beside the finding counts by type
and the counts added over the unplanted copy. Both planted cleanly against
the two novels (10 faults in 8 and 7 files; 10 canon statements each).
`test_observer_faults` pins the offsets, the shared anchor, the refusal on an
ambiguous anchor and the scorer.

The scored runs, each on an idle machine on AC power with no other
`llama-server` alive: for each novel, the unplanted copy and the planted copy
with its canon statements, through `observer_replay --real`; about one hour
a copy for *Mars* and two for *Pride and Prejudice* at 22 s a job. Recall by
type comes from the planted copies; the precision audit's fixed-seed sample
comes from the unplanted ones.

## Step 3, done: *A Princess of Mars*, scored

**Runs** (2026-11-13, M4 on AC power, nothing else of ours running; the
model's slot context 8192). The unplanted copy ran 18:36:46Z to 19:35:04Z,
3,498 s: 569 findings, 143 jobs, 143 model calls, status "Up to date". The
planted copy with its ten canon statements started 19:35:34Z and stopped at
the limit after 6,066 s with status "Reading ch27.md…": 595 findings, 140
jobs, 898 model calls (140 extractions and 758 judge calls, 5.4 a job: each
statement is judged against up to three canon entries in two passes). It
had read ch01 to ch26 whole and three findings into ch27 (the unplanted run:
13 in ch27, 10 in ch28). All ten faults lie in ch02 to ch21, so recall is
scored on finished chapters; the "added" counts below are not comparable for
ch27 and ch28 and are read with that. The hosted acceptance run for `452a6ae`
shared the machine from 20:28Z to 20:36Z, inside the planted run; the limit
it hit was 5,400 ticks of at least 50 ms, which ran 12% over the 5,400 s it
was meant to be.

**Three harness defects, found by the first scoring and fixed before a number
was trusted.** None touches the fixture, which is unchanged.

1. The scorer crashed: the real model writes subjects and contents with
   newlines in them, and the replay printed them raw, so two findings of the
   unplanted run and three of the planted spanned several physical lines.
   `observer_replay.c` now escapes a newline, carriage return, tab, pipe or
   backslash inside a field, and the two tools read through
   `tools/observer_lines.py`, which unescapes and joins the physical lines of
   a finding written before the escaping, reporting what it joined (LINES
   lines in the scorer's output: 571 physical lines, 569 records, 2 joined;
   599, 595, 3). The fake path is byte-identical with the new emitter
   (`observer_replay_run.sh escfix`: pp, mars, heights identical).
2. The units did not match. The planter recorded byte offsets, believing the
   observer's source spans were bytes. A conflict finding quoting the M8
   sentence word for word scored as "not on a fault"; checked over every
   finding of the planted run, all 595 slice to their quote by characters and
   491 of them would not by bytes (the novel's curly quotation marks are
   three bytes each). `SourceRef.start` and `len` count code points. The
   planter now records both (`offset`/`length` in bytes for slicing the
   file, `char_offset`/`char_length` for the spans) and the scorer compares
   characters; the test plants after curly quotes so the two differ by four
   and pins that the byte offset does not match. With bytes the first
   reading was 3 of 10; with characters it is 5 of 10. The record was
   regenerated from the committed fixture and the planted files compared
   byte for byte with the run's copies before rescoring.
3. The replay's limit counted ticks, not seconds; it is wall time now and
   the summary line ends with the elapsed seconds.

**Recall by type, planted copy: 5 of 10.**

| type | found / planted | found | missed |
|---|---|---|---|
| place | 3 / 3 | M3, M6, M9 | |
| event-order | 1 / 1 | M10 | |
| kinship | 1 / 2 | M8 | M2 |
| timeline | 0 / 2 | | M4, M5 |
| appearance | 0 / 2 | | M1, M7 |

Why the five were missed, from the findings on each planted sentence:

- **M4** (timeline, "the third of May", ch02) shares its anchor with M3. One
  conflict finding covers both inserted sentences in its quote and is
  credited to M3 only, as the scorer's one-finding-one-fault rule requires;
  its content read `Canon states Captain James K.`: the judge's explanation
  cut at the initial by the first-sentence rule (fixed below). The judge saw
  the pair and fired once.
- **M2** (kinship, "Dejah Thoris was the daughter of Tardos Mors himself",
  ch17): an entity finding on the sentence records "Tardos Mors — the
  grandfather of Dejah Thoris" (true of the book, from the canon or the
  chapter), but no statement about her parentage reached the judge as a
  conflict.
- **M1** (appearance, the green Martians' eyes, ch07), **M7** (appearance,
  "her golden hair", ch13) and **M5** (timeline, "two years after that
  night", ch19): no finding of any type on the sentence. The extraction did
  not produce a statement from these sentences, so the judge never saw them.
  Appearance and timeline faults sit in subordinate descriptive sentences;
  the extraction's "at most 8 findings" a chunk goes to events and names.

Recall's limit on this novel is extraction, not the judge: four of the five
misses never reached a judge call.

**The conflict path's precision, on the planted copy.** 49 conflict findings,
where the unplanted copy (no canon) had none: 5 on a planted fault, 44 on no
fault. By canon entry cited: Kantos Kan's thirty days to Helium 25, the
Princess of Helium 15, the padwar of Helium 3, Powell's March 3 3, Sola's
parentage, Powell of Richmond and the Apaches' terror 1 each. Twelve of the
44 read in full: every one is the judge calling "conflict" on a statement
that *adds* something about the canon entry's subject (Dejah Thoris wears
silks; Kantos Kan is imprisoned beneath the palace; Tardos Mors greets John
Carter) and none is a contradiction. Both passes agreed in all 49, since the
two-vote rule is the only route to a finding. On this copy the conflict
path's precision is 5 of 49, about 0.10: an author with ten canon entries
over this novel would be shown 44 false alarms beside 5 true ones, each with
its canon entry and quote. That is the finding of this step for the product,
and the remedy is structural, to be chosen and then measured on both novels:

- the judge's verdict carries the two values it says disagree, and software
  accepts the conflict only when each value occurs in its side's text (the
  fake judge already works this way, by attribute pattern);
- or a software gate before the model: the statement and the canon entry
  must share a subject and an attribute word before a judge call is spent
  (this also cuts the 758 judge calls, 84% of the planted run's model time);
- or the pane shows conflicts under a "possible" heading with the number of
  canon checks behind them, which discloses but does not fix.

None of these is a prompt tweak; each is measured by this harness before it
ships (the per-type recall must not fall, the off-fault count must).

**One product change, small, from this step.** The conflict content's
first-sentence cut (`plain_check`) ended a sentence at any period followed by
a space and a capital, so `Canon states Captain James K. Powell is of
Richmond.` became `Canon states Captain James K.`. `obs_first_sentence`
(observer_text.c) now skips a period after an initial or a title (Mr, Mrs,
Dr, Capt, …); `test_observer_text` pins eight cases. The fake path's findings
are byte-identical with the change (`observer_replay_run.sh firstsent`, see
Gates).

**Precision by type, audited (unplanted copy).** Seed 20261113, 40 a type
where that many exist: 40 entity of 302, 40 event of 262, the 1 question,
the 4 relationships; 85 lines (`tools/observer_sample.py 20261113 40
build/observer/real-mars-base.txt`, the sheet reproduced by that command).
Audited by Claude from each line's quote and the novel's text, two claims
beyond their quote checked by search in the chapter (no gold anklet in ch13;
"ten thousand soldiers of Helium" in ch27). The author can re-audit from the
same seed; the verdicts are below, line by line.

| type | sampled | right | wrong | empty | precision (right / sampled) |
|---|---|---|---|---|---|
| entity | 40 | 19 | 12 | 9 | 0.48 |
| event | 40 | 34 | 6 | 0 | 0.85 |
| question | 1 | 0 | 1 | 0 | 0.00 |
| relationship | 4 | 3 | 0 | 1 | 0.75 |

Entity verdicts by record index (the sheet's second column): right 4, 7, 9,
15, 25, 26, 66, 68, 84, 91, 133, 155, 156, 160, 182, 186, 202, 225, 282.
Wrong 24 (Dejah Thoris conflated with the palace, "possibly dead in an
incubator"), 33 (Helium as John Carter's home, Sola a princess), 36 (Helium
"five hundred miles from Barsoom"), 48 ("John Carter (mother of the further
moon)", an oath read as a name), 56 ("too astute to be caught by a caravan",
meaning inverted), 67 (a people and one warrior conflated), 153 (braves
"regard the narrator with awe", not in the text), 170 ("cowardice… a cause
of the paralysis"), 234 ("her fifty Tharks"), 246 (the jailer "does not vouch
for" the food: "vouchsafe" misread), 249 ("the mount is a replica of little
devils"), 263 ("the narrator's grandfather's territory": Dejah Thoris's).
Empty 138, 144, 200, 201, 222, 255, 258, 265, 268 (the narrator under a
pronoun twice, a sentence fragment as a description, "the other buildings",
"the nineteen others").

Event verdicts: wrong 315 ("she leaves Dejah Thoris": she is Dejah Thoris),
338 ("ten warriors": six), 343 ("searches the upper rim of the sun"), 355
("strikes the narrator's breast": the point was at his breast; "Kaor" is a
greeting), 400 ("I draw back my sword": sheathing it and advancing), 442 (a
gold anklet the chapter does not contain). The other 34 right, five of them
with a clumsy or garbled subject (338, 397, 448, 495 carry a stray CJK
character or a cut word) and a true description.

Question 564 wrong: the text asks why she is happy when he is with her; the
finding says "whether she feels safe and will soon return to her father's
court". Relationships: 566, 567, 568 right; 565 empty (a relationship with
no second entity named).

Reading the wrong ones: pronoun and attribution errors (whose grandfather,
whose Tharks, who leaves whom) are the largest class, then a phrase read as
a name or a thing (the oath; "cowardice"), then a clause misread against the
quote. Events, which the model copies closely from a sentence, are right
seven times in eight; entities, which it must name and characterise, under
one in two with a further quarter empty.

**Thresholds.** These are the first scored numbers, the author sets the line
(the harness section above). What they say as they stand: planted-fault
recall 0.50 over all types, 1.00 for place and event order, 0.00 for
appearance and timeline; conflict precision about 0.10 with ten canon
entries; entity precision 0.48, event precision 0.85. *Pride and Prejudice*
runs next, same harness, before anything is tuned.

## Step 4, done: *Pride and Prejudice*, scored

**Runs** (2026-11-13/14, started after the hosted runs for `5e7e9b8` had
finished, so no second model shared the Mac; slot context 8192 both). The
unplanted copy started 21:47:57Z and ran 6,092 s: 688 findings, 268 jobs, 268
model calls, "Up to date" (22.7 s a job; *Mars* 24.5). The planted copy with
its ten canon statements started 23:29:59Z and ran 13,891 s: 732 findings, 268
jobs, 2,392 model calls (268 extractions and 2,124 judge calls, 7.9 a job),
"Up to date". The Mac went to battery at 00:50Z (the power log) and ended at
30% at 03:21Z, so the planted run's last two and a half hours were on
battery; its wall time is not comparable with the unplanted run's, and
recall and content are not timing (the base run was on AC throughout). Both
files parsed whole: 688 and 732 physical lines, as many records, none
joined, so the escaping emitter did its work on this model's newlines.

**Recall by type, planted copy: 2 of 10.**

| type | found / planted | found | missed |
|---|---|---|---|
| place | 1 / 1 | P3 | |
| kinship | 1 / 5 | P9 | P1, P4, P5, P7 |
| timeline | 0 / 2 | | P2, P6 |
| appearance | 0 / 1 | | P8 |
| event-order | 0 / 1 | | P10 |

Where the eight misses stopped:

- **Never extracted** (no finding of any type on the planted sentence):
  P2 (Bingley's twelve thousand a year), P4 (Collins as Mrs. Bennet's
  brother), P5 (three daughters), P6 (married above forty years), P8
  (Charlotte barely nineteen), P10 (Mrs. Gardiner never in Derbyshire). Six
  of eight.
- **Extracted, not judged a conflict:** P1 ("Lydia, as the eldest of the
  five sisters") became an event finding whose content repeats the planted
  claim, "Lydia, as the eldest of five sisters … presided over the other",
  beside the canon "Lydia is the youngest of the Bennet sisters"; P7 ("Mr.
  Gardiner, Mr. Bennet's younger brother") became an event finding that
  garbles it ("sent by Mr. Bennet's younger brother") beside the canon "Mr.
  Gardiner is Mrs. Bennet's brother". The judge had both pairs in reach and
  passed them.

Over both novels: 7 of 20 planted faults found (place 4/4, event-order 1/2,
kinship 2/7, timeline 0/4, appearance 0/3). Of the 13 misses, 9 were never
extracted, 3 were extracted and passed by the judge, 1 was credited to the
fault sharing its anchor. Numbers in a subordinate clause (an income, a
count of daughters, an age, a span of years) are what the extraction leaves
out; it takes the clause's event or person and drops the figure.

**The conflict path's precision, planted copy.** 43 conflict findings, the
unplanted copy none: 2 on a fault, 41 on no fault. By canon entry cited:
Lady Catherine the aunt 19, Pemberley in Derbyshire 8, married
three-and-twenty years 6, Mr. Gardiner the brother 5, Charlotte's age 3,
Lydia the youngest 2. Twelve of the 41 read: as on *Mars*, none is a
contradiction, and here most pair a canon entry with a statement about
someone else entirely ("Canon: Charlotte Lucas is about twenty-seven. the
new text: Maria Lucas — a young woman…"; "Canon: Lady Catherine de Bourgh is
Mr. Darcy's aunt. the new text: Mr. Wickham — the man whose removal from the
corps…"). The candidate step (`job_canon_candidates`, the two best entries by
shared words) sends the judge pairs that share a surname or a title, and
the judge, twice, calls the pair a conflict. Over both novels the conflict
path's precision is 7 of 92, about 0.08, and the judge calls took the
planted runs from 3,498 to 6,066 s and from 6,092 to 13,891 s. The three
structural remedies of step 3 stand; the second (a software gate before the
model: shared subject *and* attribute) addresses exactly what these twelve
show, and the first (values that must occur in each side's text) would have
refused all twelve.

**A second cut at the first sentence.** 7 of the 43 conflict contents end
"the new text introduces 'Mrs." or "'Mr.": the title opened a quotation, and
the fix of step 3 compared the word with its opening quote mark. The word
before the period is now read past an opening quote, bracket or asterisk;
`test_observer_text` adds the three cases. Fake-path yardstick and gates in
the Gates section (`firstsent2`).

**Precision by type, audited (unplanted copy).** Seed 20261113, 40 a type
where that many exist: 40 entity of 354, 40 event of 309, the 15 questions,
the 10 relationships; 105 lines (`tools/observer_sample.py 20261113 40
build/observer/real-pp-base.txt`). Audited by Claude from each line's quote
and the novel's text, with two passages read in the chapter (the question
Mr. Bennet does not answer is Jane's or Elizabeth's, not Mr. Gardiner's;
Elizabeth is met by her father on the stairs before the fireplace).

| type | sampled | right | wrong | empty | precision (right / sampled) |
|---|---|---|---|---|---|
| entity | 40 | 15 | 19 | 6 | 0.38 |
| event | 40 | 26 | 14 | 0 | 0.65 |
| question | 15 | 7 | 6 | 2 | 0.47 |
| relationship | 10 | 6 | 4 | 0 | 0.60 |

Entity verdicts by record index: right 4, 15, 33, 48, 133, 144, 153, 156,
182, 200, 222, 246, 249, 258, 263. Wrong 7 (Bingley "advises Elizabeth and
returns to his house": the quote says he followed advice), 24 ("Charlotte
Longbourn", "the match of Lucas and Elizabeth"), 26 ("Charlotte Lucas (Mrs.
Collins' husband)", the instrument a marriage), 36 ("Darcy (Fitzwilliam)",
two people), 56 ("Elizabeth (the narrator's friend)", a description the
quote does not hold), 66 (Jane intends to visit Epsom: Mrs. Bennet's plan for
her husband), 68 (Fitzwilliam's sentence inverted), 138 (a description the
quote does not hold), 155 ("Lydia's family (sister) — Mr. Gardiner's sister"
from "How is half such a sum to be repaid?"), 170 ("Miss Bennet (Elizabeth's
mother)"), 186 (Miss De Bourgh "a possible match in his neighbourhood"), 201
(Mr. Bennet "does not answer Mr. Gardiner's question": Jane's or
Elizabeth's), 234 ("Mrs. (Lady) Bennet / 'your sister'": Jane), 255 (Mrs.
Jenkinson "at Hunsford": Rosings), 282 ("Sarah Bennet — another daughter":
the maid), 307 ("Wickham's refusal of Darcy"), 334 (the housekeeper "takes
over after Mrs. Bennet vents her feelings on her absence"), 350 ("your own
case": Darcy's letter on Jane's marriage read as "the narrator's previous
marriage decision"), 351 ("your resentment": Elizabeth's, read as the
narrator's). Empty 9 ("Brotherly pride"), 67 ("February"), 84 ("Her aunt"),
91 ("I"), 326 ("muslin"), 347 ("you (the other person)").

Event verdicts: right 354, 363, 367, 379, 392, 401, 404, 418, 424, 427,
434, 437, 460, 481, 494, 514, 520, 551, 556, 579, 596, 616, 620, 622, 631,
651. Wrong 386 (Sir William "striking Darcy forcibly": the allusion struck
him), 400 (Elizabeth's question put to Darcy: to Lady Catherine), 465
(Elizabeth's acquaintance with Bingley: Jane's), 477 ("stood by Mrs.
Annesley": by Elizabeth), 500 ("journey to Rosings": to Mr. Gardiner's), 516
("Miss Bingley's explanation": Darcy's), 534 (Bingley saw Wickham "the
morning after he meets Darcy"), 544 (Darcy an inmate of Bingley's house:
inverted), 567 (Lizzy blamed "for possibly marrying Collins": for refusing
him), 568 (Mrs. Bennet "must go to Newcastle": Lydia), 576 ("Mr. Gardiner's
return": Mr. Bennet's), 592 ("Proposal of Mr. Collins and Lady Lucas"), 611
(a conditional read as a decision), 619 (Lady Catherine deciding against
him: Elizabeth acknowledging to her).

Question verdicts: right 665, 666, 668, 670, 671, 672, 676. Wrong 663 (a
clause the quote does not hold), 669 (Jane at Pemberley), 673 ("He asks":
Elizabeth asks her father), 674 (she went to meet him: he came a day early),
675 (Lydia at the wedding: Darcy), 677 (Bingley's manners: Darcy's). Empty
664 (a dread, not a question), 667 (asked and answered in the next line).
Relationship verdicts: right 678, 679, 680, 684, 685, 687. Wrong 681
("opposite side", "cold toward her"), 682 (who will not let whom smile,
inverted), 683 (Darcy's "sense of her inferiority" read as his own), 686
(the brother named as Mr. Bennet; "agrees to go to Gracechurch Street",
where he lives).

Reading the wrong ones: attribution and inversion again lead (who speaks,
who acts on whom, whose feeling it is: 544, 683, 682, 567, 400, 465, 477,
576, 619, 673, 674, 677), then a misnamed subject (Charlotte Longbourn,
Sarah Bennet a daughter, Miss Bennet a mother, Darcy (Fitzwilliam)), then
the letter voice (Darcy's letter read as "the narrator"). *Pride and
Prejudice* is dialogue and free indirect speech, and the model loses the
speaker; *Mars* is first-person narration, and it fared better on every
type. Over both novels: entity 34 of 80 right (0.43), event 60 of 80
(0.75), question 7 of 16, relationship 9 of 14.

**The numbers on the table for the author.** Two novels, same harness,
nothing tuned:

| measure | Mars | Pride and Prejudice | both |
|---|---|---|---|
| planted-fault recall | 5 / 10 | 2 / 10 | 7 / 20 |
| … place | 3 / 3 | 1 / 1 | 4 / 4 |
| … event-order | 1 / 1 | 0 / 1 | 1 / 2 |
| … kinship | 1 / 2 | 1 / 5 | 2 / 7 |
| … timeline | 0 / 2 | 0 / 2 | 0 / 4 |
| … appearance | 0 / 2 | 0 / 1 | 0 / 3 |
| conflict findings true / shown | 5 / 49 | 2 / 43 | 7 / 92 |
| entity precision (empty) | 0.48 (9/40) | 0.38 (6/40) | 0.43 |
| event precision | 0.85 | 0.65 | 0.75 |
| question precision | 0 / 1 | 7 / 15 | 7 / 16 |
| relationship precision | 3 / 4 | 6 / 10 | 9 / 14 |

The author sets the thresholds from these; the first change measured
against them should be the conflict path's gate, which the harness scores
in about seven hours of model time for both novels.

**Thresholds, agreed 2026-11-14.** The author asked for them to be set from
the tests ("as you see fit according to the tests"); set from the two-novel
table, each a floor a change must meet or raise, and no row may fall below
its value today when a change ships:

| measure | today | threshold |
|---|---|---|
| planted-fault recall, both novels | 7 / 20 | at least 0.50 |
| … place | 4 / 4 | 1.00 |
| conflict findings true of shown | 7 / 92 (0.08) | at least 0.50 |
| entity precision | 0.43 | at least 0.60 |
| … empty entities | 15 / 80 (0.19) | at most 0.10 |
| event precision | 0.75 | at least 0.80 |
| question precision | 7 / 16 | at least 0.60 |
| relationship precision | 9 / 14 | at least 0.75 |

The remedy chosen for the conflict path (the author: "what makes the most
sense"): two software changes measured together, a candidate step that does
not count a shared title or honorific (Mr, Mrs, Miss, Lady, Sir, Captain…)
as a shared name, and a judge whose verdict carries the two values it says
disagree, accepted only when each value occurs in its side's text. The
planted copies of both novels are rerun with them; recall must hold at 7 of
20 or rise and the off-fault conflicts must fall from 85.

## Step 5, done: the conflict gate, measured and gated off

The remedy chosen in step 4 was built as two software changes and measured
together on the planted copies of both novels, each scored against its
unplanted run from steps 3 and 4 (`build/observer/real-conflict.log`,
`real-mars-planted2.txt`, `real-pp-planted2.txt`): the canon candidate step
ignoring a shared title (`obs_title`, 42 titles from Mr to earl, skipped in
`tokens()` beside the stopwords) and a judge whose constrained reply carries
`canon_value` and `new_value` before its verdict, a conflict vote accepted
only when each value occurs in its side's text and the two differ
(`obs_values_in_texts`). The fake-path yardstick was byte-identical with
`realflag` on all three novels (717 / 348 / 948), so the fake judge's
attribute-pattern values already satisfied the check; the real model was the
question.

| run | wall | reached | findings | conflicts | on a fault | off-fault | recall |
|---|---|---|---|---|---|---|---|
| Mars planted, step 3 | 6,066 s (tick limit) | ch27 of 28 | 595 | 49 | 5 | 44 | 5 / 10 |
| Mars planted, gate | 7,201 s (limit) | ch25 of 28 | 590 | 64 | 3 | 61 | 3 / 10 |
| P&P planted, step 4 | 13,889 s (complete) | part 8 of 8 | 732 | 43 | 2 | 41 | 2 / 10 |
| P&P planted, gate | 14,400 s (limit) | part 7 of 8 | 677 | 85 | 2 | 83 | 2 / 10 |

Both targets failed. Off-fault conflicts rose on both novels, 85 to 144 over
the two, although each gate run read less text than its baseline (the Mars
run stopped two chapters short, the P&P run one part short, every planted
fault but P7 and P10 inside the text read). Recall fell on Mars from 5 to 3
(M3 Powell's Richmond, M4 Powell's March 3 and M10 Helium in three days
found; M6 the padwar of Zodanga, M8 Sola's parentage and M9 the Princess of
Zodanga lost; M1, M2, M5, M7 missed as before) and held at P3 and P9 on
P&P. The found contents are the same shape as before: `Canon states Captain
James K. Powell is of Richmond. the new text introduces a quote about
'Powell' who was born and raised in Boston`.

What the 144 off-fault records show, read in the sample above and in the
first fourteen of each file: the same additions as in steps 3 and 4 (Dejah
Thoris grasps the narrator's hand; Charlotte says she hardly ever does),
each now with a canon value and a new value the model named and software
found in the texts. The check as built verifies only that two strings occur,
one in each text, and differ; "Princess of Helium" and "promised her hand to
Sab Than" pass it. One ch25 record flags a sentence that restates the canon
(*Dejah Thoris would be joined forever to the Prince of Zodanga*, canon
*Dejah Thoris is the Princess of Helium*). The rise rather than a fall is
consistent with the two required value fields, filled before the verdict,
pushing the verdict toward "conflict": a model that has just written two
values says they disagree. That is a reading, not a measurement; the replay
did not record the judge's replies, so which rule on the values would have
rejected which record cannot be scored from this run, and the stoplist's
share cannot be separated from the judge's. M9's lost fault is the one case
the stoplist alone explains: its insert, *She was, after all, the Princess
of Zodanga*, shares only "Princess" with its canon entry, and "princess" is
on the title list.

**Decision taken by the rule for failed experiments** (MEMORY-015 §5, delete
nothing, only archive): both halves stay in the tree behind one flag,
`SCRIBODO_OBSERVER_CONFLICT_GATE=1`, a field of `Experiments`
(`src/core/experiments.h`), default off. Off, the candidate step and the
judge are the step 4 code (titles count as shared names; the judge's reply is
`check` and `verdict`, 260 tokens). On, they are the measured gate. The
default is the measured choice: the observer ships today at 7 true in 92
conflicts, and the threshold of 0.50 stands unmet. `test_observer_text`
pins both states (the Darcy/Wickham pair overlaps 2 by default and 0 under
the flag; the Helium/Zodanga pair likewise), reloading the table around the
flag so the test asserts the default whatever the environment.

**What the next attempt needs first, in software, before any model time:**
the replay records every judge call (canon, statement, order, check, the two
values, verdict, and whether the statement lies on a planted fault) to a
file, so that candidate rules on the values (a value may not restate the
other text; the two values must be of one kind, a number, a name, a kinship
word, a colour; the values must not be the shared subject) are scored
offline in seconds against a run's 2,000 real replies, as the composer's
request replays are (journal/answer-cycle3). A rule that scores well on the
recorded replies is then measured once on both planted copies. Order of the
schema's fields (values after the verdict) is a prompt change and is
measured the same way, never shipped on the reading above.

Run conditions, for the record: the Mac was on AC power at 38% when the Mars
run began (04:56Z) and on battery, 13%, when the P&P run ended (10:57Z); the
hosted acceptance runner was idle throughout (the chain waited for it).

## Gates

On this tree (`build/observer/realflag-gates/gates-*.log`, `gates-exit.txt`):
`make test` 95,994 C checks and 186 Python tests, 0 failures; `make analyze`
0 warnings across 87 source files; `make sanitize` 95,994, 0; `make tsan`
48,624, 0; `make dbtest` 18,000, 0; every exit status 0. Hosted: CI and
Trusted acceptance passed on `7622319` (runs 43481524642 and 43481524637).
Step 2 (Python tools and their test, no C change): `make test` 95,994 C
checks and 189 Python tests, 0 failures, exit 0 (`build/observer/faults-gates/`);
hosted: CI and Trusted acceptance passed on `9bcfe0b` (runs 43482838534 and
43482838639).
Step 3 (harness tools in Python, the replay's emitter and limit, the first-sentence fix in C), on the tree
with all of it (`build/observer/cycle2-gates/`, `summary.txt`): `make test` exit 0, per-binary tallies summed
95,707 C checks and 192 Python tests, 0 failures (this sum is of the "N checks, M failures" lines; it is the
method of this entry, 8 more than the step 2 tree by the same method for `test_first_sentence`); `make analyze`
0 warnings across 87 source files; `make sanitize` 95,707, 0; `make tsan` 48,329, 0; `make dbtest` 18,000, 0;
every exit status 0. Yardsticks: `observer_replay_run.sh escfix` (the escaping emitter) and `firstsent` (the
first-sentence fix) both byte-identical with `realflag` on pp, mars and heights (717, 348, 948).
Step 4 (the second first-sentence fix in C, the journal and docs), on the tree with all of it
(`build/observer/cycle2b-gates/`, `summary.txt`): `make test` exit 0, per-binary tallies summed 95,710 C checks
(3 more for the new cases) and 192 Python tests, 0 failures; `make analyze` 0 warnings across 87 source files;
`make sanitize` 95,710, 0; `make tsan` 48,329, 0; `make dbtest` 18,000, 0; every exit status 0. Yardstick
`observer_replay_run.sh firstsent2` byte-identical with `realflag` on pp, mars and heights (717, 348, 948).
Hosted: CI and Trusted acceptance passed on `452a6ae` (runs 43501766672 and 43501766788), on `5e7e9b8`
with `e290e7d` (43512619083 and 43512619064) and on `44d8c0f` with `754ac0d` (43552119722 and 43552119645).
