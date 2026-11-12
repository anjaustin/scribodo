# Answer cycle 4, item one: the composer window and earlier refusals

**Date:** 2026-11-11. **Tree:** `5eee9eb` plus the changes recorded below.
**Direction:** "Start on window construction, item one."

## The mechanism, as recorded before this work

The verified pipeline's composer sees a six-message window of the conversation
(`chat_build_request_pipeline`: the newest three pairs, starting at a user
turn, with the verified block in the system message). Pass 4 recorded that a
withheld notice stored as the assistant's turn is imitated on the turns that
follow; cycle 3 measured it as the dominant cost of withholding (every run
with a notice before the Charlotte and walk turns had both abstaining in the
notice's words, the one run without had neither); cycle 4 saw the composer
echo its own earlier abstention. The composer instruction itself supplies the
sentence ("say that the supplied manuscript passages do not give it"), so the
wording is the instruction's; what the window adds is the example.

One correction to cycle 4's reading before anything else: the Mars correction
reply that opened with the abstention sentence had **no refusal in its
window** (`build/mars/capture-pipe1/request-5.json`: the Thark answer, the
author's note, the acknowledgement). That opening was the instruction's
phrase, not an imitation. The window finding stands for the cases where a
refusal is in the window; it does not explain every occurrence of the phrase.

## Measuring the window offline

The live capture of a run holds every request the composer was sent
(`request-N.json`) and every reply (`content-N.txt`). A request is a fixed
input; regenerating it on the same model with the same sampling fields and
seed either reproduces the reply or does not. `tools/answer_window_replay.py`
takes every captured request whose window holds a refusal (a withheld notice
or an abstention, recognised by its opening) and regenerates it under four
arms, changing only the history:

| Arm | The refusal assistant turn becomes |
|---|---|
| `recorded` | itself (the fidelity control) |
| `marker` | one neutral line, `(No answer was given.)` |
| `drop_turn` | nothing; its question stays |
| `drop_pair` | nothing; its question goes too |

Inputs: the coref1 and coref3 runs of the first novel and the mars1 run of the
second; 18 requests with a refusal in the window, 72 generations, the pinned
BF16 model under the app's server flags
(`build/window-replay/wr1`, manifest with model, server and tool hashes).

**Fidelity: 18 of 18 `recorded` arms reproduced the recorded reply byte for
byte.** Every difference between arms below is therefore the window, not
sampling noise.

### Question turns (twelve with designated passages)

| Arm | Replies that are not refusals | Replies carrying every designated passage | Both |
|---|---:|---:|---:|
| recorded | 7 / 12 | 8 | 4 |
| marker | **11 / 12** | 7 | **6** |
| drop_turn | 10 / 12 | 4 | 4 |
| drop_pair | 8 / 12 | 7 | 4 |

Case by case, the three that moved:

- **Walk, both runs** (`coref1` r3, `coref3` r3). Recorded: a refusal that
  quotes "only three miles" and calls its context unclear. Marker: *"Elizabeth
  says the walk to Netherfield is only three miles."*, sourced, in both runs.
  Drop arms: refusals again, with the passage quoted and hedged.
- **Fortunes** (`coref3` r1). Recorded: the Bennet misattribution. Marker:
  Source 1 for Mr. Bingley's hundred thousand, Source 2 for the sisters'
  twenty thousand, both quoted, both right.
- **Charlotte, both runs**: refuses or hedges under every arm. Its span reads
  "The eldest of them, a sensible, intelligent young woman, about twenty-seven,
  was Elizabeth’s intimate friend", and the composer reads the age as
  Elizabeth's under every window. This is not a window case; it is the
  pronoun-subject attribution class of cycle 3 on the output side, and it
  moves to the relation item.

Unchanged under every arm: the correction (hedges), the drafts (right),
unavailable (refuses, rightly), creative (writes).

### Note turns (three: "Note for later", "Draft note (older)")

Recorded: the composer acknowledges the note in every case. Marker and
drop_turn: in every case the composer **answers the earlier, now-open
question instead**: *"The note says Lydia is seventeen, but it does not
mention Elizabeth's age or how far she says the walk to Netherfield is"*. The
recorded refusal had closed that question; the marker reopens it. Drop_pair,
which removes the question too, acknowledges the note as recorded.

## The rule this measures into

Two findings, one rule:

1. On a question turn, an earlier refusal in the window is an example the
   composer follows; a neutral marker in its place lets the composer answer
   the current question from its spans, and in this sample it answered three
   of them correctly where it had refused or misattributed.
2. On a note turn, the same marker leaves an earlier question open and the
   composer answers it instead of the note.

So the window carries earlier refusals as the marker **only when the current
turn is a gated question** (`answer_gate_applies`), and as recorded otherwise.
A refusal is recognised from the stored turn's receipt
(`answer_receipt_refusal`: the ANSWER GATE line says `withheld=1` or
`verdict=no_answer`), never from the reply's wording, so the rule reads the
record the gate wrote rather than guessing from text; a turn with no receipt
(no gate ran) is never a refusal. The visible reply and the stored record are
unchanged; only what the composer is shown differs.

What this does not measure: the rule changes later windows (a question now
answered is an answer in the next window, not a refusal), which only a live
run shows. That is the next section.

## Implementation

- `chat_receipt_refusal` (`src/llm/chat_history.c`) reads the ANSWER GATE
  line of a stored turn's provenance: `withheld=1` or `verdict=no_answer` is a
  refusal; anything else, or no receipt, is not.
- `chat_request_history_copy` sets a new `ChatMessage.refused` from that
  receipt while copying a turn for a request, and still strips the provenance
  text itself, so the rule "retrieval evidence is never model history" holds:
  one derived bit travels, nothing else.
- `chat_build_request_pipeline` (`src/llm/chat.c`), when the newest turn is a
  question (`answer_gate_applies`), shows every refused assistant turn in its
  window as `ANSWER_WINDOW_MARKER`, "(No answer was given.)"; on any other
  turn the window is as recorded. The copy is local to the request; the
  session's messages and the stored record are untouched.
- Tests: `test_answer_pipeline` (the marker on a question turn, the recorded
  text on a note turn, a turn without a receipt and an answered turn left as
  recorded, the record untouched), `test_chat_history` (the flag derived and
  the provenance dropped). 211 and 18,103 checks.

### A first live attempt that measured nothing, kept

The first instrumented build with the rule (`build/answer-live-window`, runs
`pipe-window1` on the first novel in the coref3 configuration and `mars-pipe2`
on the second) produced replies **byte-identical** to coref3 and mars-pipe1 on
every turn, and no captured request contained the marker. The rule had read
`memory_sources` on the window's messages, and `chat_request_history_copy`
sets that field to NULL by design before the request is built; the receipts
were never there to read. The runs are kept as the control they turned out to
be: the rule off, same tree, same replies, so a later difference is the rule.
The derived flag above is the fix.

## Live, with the rule (`pipe-window2`, `mars-pipe4`)

Instrumented build `build/answer-live-window3` from the final tree. The first
novel in the coref3 configuration (verified pipeline plus the FCoref
sidecar, seed 42), 537 s; the second novel as in mars1, 352 s. Review sheets
`build/answer-pipe-capture/review-pipeline-window2.json` and
`review-pipeline-mars4.json`. Selection on both novels is byte-identical to
coref3 and mars1 (same designated ranks, same two source-delivery misses
each), so every difference below is the window.

**The marker was sent on three first-novel turns** (fortunes, after the entail
notice; Charlotte, after two notices; walk, after Charlotte's abstention) and
on **one second-novel turn** (Thark arrival, after the Kantos Kan abstention).

**The offline replay predicted the live composer.** The live fortunes request
is the offline `coref3` r1 marker arm with the same history, and the
composer's raw output is byte-identical to the offline reply
(`build/window/capture-pp3/content-1.txt` against
`build/window-replay/wr1/coref3-r1-marker-response.json`). The captured
request is a fixed input; the method holds.

### First novel, turn by turn against coref3

| Case | coref3 | pipe-window2 | Window |
|---|---|---|---|
| entail | withheld (quotes "sons") | identical reply, withheld | no refusal yet |
| fortunes | withheld: Bennet misattribution, misquote | composer gives both fortunes to the right parties ("Mr. Bingley's sisters: twenty thousand pounds"; Bingley "nearly a hundred thousand"), then adds a wrong claim ("inherited Netherfield") and a corrupted quotation ("more than they his ought"); **withheld by the quotation check**, rightly | marker |
| Charlotte | abstains in the notice's words | abstains in its own words, inside parentheses like the marker, same misreading ("about twenty-seven, was Elizabeth's intimate friend" read as Elizabeth's age) | marker |
| walk | abstains in the notice's words, "only three miles" quoted and called unclear | **"Elizabeth says the walk to Netherfield is only three miles."**, sourced; gate pass, quotations clean | marker |
| note (Lydia) | acknowledged | acknowledged | as recorded (note turn) |
| correction | "fifteen", correct | "fifteen", correct and sourced, then a hedge that the text gives no definitive answer | no refusal in window |
| drafts note | acknowledged | acknowledged, with a stray source line | as recorded |
| drafts | withheld (own commentary in quotes) | right conclusion ("the current manuscript gives three miles, and the other draft claims five miles") after a preamble that labels the two drafts the wrong way round; gate pass | no refusal in window |
| unavailable | refuses, rightly | refuses, rightly | marker not needed |
| creative | writes | writes | |

| | coref3 | pipe-window2 |
|---|---:|---:|
| Correct answers shown | 1 | **2** |
| Shown with a wrong clause beside a right conclusion | 0 | 1 (drafts labels) |
| Wrong replies withheld | 3 | 2 |
| Abstentions on good evidence | 2 | 1 (Charlotte) |
| Fabrications shown | 0 | 0 |

### Second novel (`mars-pipe4`, and `mars-pipe3` on the intermediate build)

All ten assistant replies byte-identical to mars1 in both runs. The one
marker turn (Thark arrival) answered as before, as the offline replay said it
would. The rule has no visible effect on this book: its only refusal with a
receipt precedes a question the composer was already answering.

## What this establishes

1. **The window rule does what the replay measured.** On the first novel the
   walk turn moves from an abstention in the notice's words to a sourced
   correct answer, and the fortunes turn moves from a misattribution to the
   right attribution. The second was still withheld, for a corrupted
   quotation the quotation check caught; the composer's attribution was right.
2. **The remaining first-novel failures are not window failures.** Entail
   (quotes "sons" for "daughters"), fortunes (a corrupted quotation and a
   wrong claim beside right numbers), Charlotte (a pronoun subject read as the
   wrong person), drafts (two labels swapped before a right conclusion): each
   is the composer changing or misreading a relation inside text it was given.
   That is the relation item, and with the window item done it is the whole of
   what is left on the "use" side of these two fixtures.
3. **Offline replay of captured requests is a measurement, not a proxy.**
   Byte-identical reproduction of recorded replies and of the live composer
   under a changed window means a window change can be measured in minutes on
   the requests a run actually sent, before any live run.
4. **Costs recorded:** one new reply with a wrong clause (drafts) where there
   was a withholding before; the marker's parenthetical shape was imitated
   once (Charlotte) without changing the outcome; wall time unchanged within
   the usual variance (537 s against coref3's 503 s).

Gates on the change: `make test` 95,840 checks and 146 Python tests, 0
failures; `make analyze` 0 warnings across 86 files; `make sanitize` 0
diagnostics; `make tsan` 48,511 checks, 0 failures, 0 diagnostics
(`build/window/gates-*.log`). Evidence archived per
[EVIDENCE](../docs/EVIDENCE.md) (`scribodo-evidence-2026-11-11-window.tar.gz`).

## Decisions that remain

1. **The relation check on the reply** is now the first open item on faithful
   use: every wrong statement shown or withheld on either novel at this tree
   is a relation between sourced words ("sons" for "daughters", "they his
   ought", the drafts' labels, Elizabeth for Charlotte, Tardos Mors for Mors
   Kajak). The quotation check is the model for how it sits beside the gate;
   whether a deterministic check can hold "X of Y" to the span is open.
   Measured and built 2026-11-12 (journal/answer-cycle4_relation.md): a
   closed-list kinship and possession check on the reply's prose flags 7 of
   60 recorded replies, the wrong relation shown among them and no correct
   answer; it sits beside the gate as the quotation check does.
2. **The marker text** was chosen once and measured once; a second wording
   is a cheap replay, not a live run, and should only be tried if a run shows
   the composer imitating the marker to its cost (Charlotte imitated its
   shape, not to its cost).
3. **The next held-out set** includes pronoun-subject attribute questions and
   relation questions by construction (unchanged from cycle 4).
