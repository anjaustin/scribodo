# Controlled observations

## Income question, component isolation

Evidence: `build/answer-bedrock-income/`. Same pinned model/runtime; diagnostic
cap 256 tokens; greedy decoding except the explicitly sampled arm. These are
reconstructed payloads, not original-run wire captures.

| Arm | Prompt tokens | Observation |
|---|---:|---|
| Minimal quotation | 155 | Gives four or five thousand a year; adds an inaccurate attribution to Bingley speaking. |
| Seven sources only | 4,099 | Confuses Darcy's ten thousand with Bingley's income; reaches diagnostic cap. |
| Full cold | 13,667 | Reproduces inheritance/property prose and document delimiters, not annual income. |
| Full cached | 13,667 | Identical generated content to full cold, with 13,411 cached tokens reported. |
| No history | 9,797 | Still gives Darcy's ten thousand as Bingley's income. |
| No document heads | 7,969 | Still repeats inheritance prose. |
| Sources after heads | 13,667 | Stops, but describes inheritance/property, not annual income. |
| Full sampled | 13,667 | Still emits unrelated narrative; reaches diagnostic cap. |

Claims broken by testing:

- Cache reuse is not necessary for this failure: the cold request fails, and
  its cached counterpart is byte-identical in generated text.
- Reordering the same sources after document heads is not sufficient.
- Removing both heads and history is not sufficient for this source packet.
- A right answer word in a source packet does not prove the designated source
  was retrieved: `four or five thousand` comes from a later marriage scene,
  not the opening-scene key. The packet also contains Darcy's ten thousand and
  Bingley's inherited capital. This is an entity/relation discrimination test,
  not simply a fact-presence test.

Template inspection: the captured rendered prompt ends with the actual current
question, the user end marker, and `assistant\n<think></think>`. A missing
generation suffix is not supported by this observation. Raw nonstreaming replies
already contain the errors, so UI streaming presentation is not required to
produce them.

Next isolation: exact opening unit versus exact later-scene unit; unchanged
packet without metadata; same target source moved last; correct opening unit
substituted as an explicitly oracle diagnostic. Longer diagnostic cap checks
whether the 256-token result merely stopped before a correction.

## Income question, evidence isolation

Evidence: `build/answer-bedrock-evidence/`; greedy, cold-cache, 512-token cap.

- Exact opening unit alone: gives four or five thousand; stops after 271 tokens.
- Exact later-scene income unit alone: gives four or five thousand; stops after
  55 tokens.
- All seven source bodies with metadata removed: wrongly gives ten thousand;
  stops after 171 tokens. Metadata is not necessary for the entity confusion.
- Same target source moved last: still does not complete the requested answer
  before the 512-token cap. Position alone is not sufficient.
- Document heads alone: correctly gives four or five thousand, stops after 185
  tokens, despite 5,811 input tokens. A simple total-token threshold does not
  explain the failure of the shorter 4,099-token source packet.
- Original source packet with longer output allowance: confidently completes
  the wrong ten-thousand answer after 314 tokens. The earlier capped output
  was not merely interrupted before a correction.
- Content-only oracle substitution of the opening passage gives the correct
  income, but retains the replaced source's wrong document metadata. It is
  **not valid provenance-preserving evidence** and cannot qualify a fix. The
  next run repeats this comparison with the real opening source binding.

Formal comparison so far:

`answer(target_unit) = requested income`

`answer(seven_source_packet) = other person's income`

`answer(document_heads) = requested income`

This demonstrates sensitivity to the evidence bundle in these cold greedy
conditions, not a universal model limit. The next test removes the specific
Darcy distractor versus a similarly sized unrelated source, and adds back the
unchanged prior transcript independently.

## Income question, matched attribution controls

Evidence: `build/answer-bedrock-attribution/`; greedy, cold-cache, 512-token cap.
All seven arms completed normally at the transport level. One reached the cap.

| Condition | Prompt tokens | Requested income stated correctly | Additional observed defect |
|---|---:|---|---|
| Correct opening unit and binding substituted, packet only | 4,101 | Yes | Includes extra quoted dialogue beyond the supplied opening unit. |
| Same corrected packet in full request | 13,669 | No | Continues narrative and document markers instead of answering. |
| Remove Darcy income unit | 3,541 | Yes | Incorrectly attributes Mrs. Bennet's line to Bingley's wife. |
| Remove similarly sized unrelated unit | 3,538 | No | Misattributes ten thousand to Collins/Bingley; reaches cap. |
| Target unit + Darcy unit, plain | 961 | Yes | Presents an altered shortened quotation. |
| Target unit + unrelated unit, plain | 965 | Yes | Misattributes Mr. Bennet's anger at Lydia to Bingley. |
| Document heads + prior transcript, no retrieval | 9,681 | No | Unrelated narrative; omits the requested income. |

The 3,541/3,538-token comparison supports a **content-specific distractor effect
for this case**, not just a total-length effect. The successful 961-token pair
also disproves a claim that merely mentioning both incomes invariably causes
failure. The surrounding source bundle matters.

The transcript comparison isolates a second effect: document heads without
history give the requested income (5,811 tokens), while the same heads with
the prior transcript do not (9,681). Replacing the retrieved passage with the
correct opening unit is sufficient for the packet-only core answer but not
for the full request. Thus a retrieval-only correction cannot be certified as
a complete fix from these observations.

Requested-value correctness is deliberately separate from whole-reply fidelity:
several numerically correct replies still invent speakers, relationships or
quotations. No such reply is silently counted as a fully grounded answer.
