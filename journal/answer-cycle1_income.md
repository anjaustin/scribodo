# Answer cycle 1 — income: selection versus execution

**Date:** 2026-11-09. **Baseline tree:** `6c243d5` plus the uncommitted AUDIT-002
changes (none touch prompts, selection, models or the capture).
**Protocol:** manifold synthesis, next executable cycle, step 1. Written before
inspecting any reply: the exact captured income request (index 1) against the
same packet with one admitted block replaced by the designated opening original
carrying its real locator. Cold greedy decoding, native 2,048-token cap, actual
8K slot. Oracle knowledge chooses the inserted unit; nothing here is a ranker.

Tool: `tools/answer_income_isolation.py` (test `tests/test_answer_income_isolation.py`,
4 no-model checks). Run: `build/ai1/`. Ran concurrently with the strict-CPU arm
`build/pq2/` on the same machine; wall times are not comparable to serial runs.

## Packets

The replaced block was chosen prospectively as the source the native live reply
quoted as its income evidence: SOURCE 2, `part01.md` unit `816c7faf…`
("Mr. Bingley inherited property to the amount of nearly a hundred thousand
pounds"). The designated original is `part01.md` unit `7640e6c3…` ("A single
man of large fortune; four or five thousand a year"), same document revision,
so only the unit id, byte range and text change in the header. The second
position arm (last admitted block, `part04.md`) was skipped by the tool because
that block's revision differs from the designated document and a synthetic
header would have been wrong.

Packet verification passed for all four arms: system and all 7 history messages
byte-identical, 7 numbered sources, designated text and locator present only in
swap arms, the quoted unit absent only there. Token accounting: baseline 5,796
prompt tokens; swap 5,831 (first changed position 1,450, +35 tokens); one-sentence
variants +5. Official template and provider token counts matched for every arm;
0 cached tokens.

Unexpected observation while reading the packet: the question's own attachment
preview is `part01.md` "(too long: cut off)", and the cut falls before the
designated sentence, so the opening original was absent from both the admitted
sources and the preview in the live run.

## Replies (native visible text; no arm changed under cleanup)

| Arm | finish | completion tokens | "four or five thousand" | "hundred thousand" (Bingley's inheritance) | "ten thousand" (Darcy's) attributed to Bingley |
|---|---|---:|---|---|---|
| baseline | stop | 889 | no | yes | yes |
| baseline + one sentence | stop | 34 | no | yes | no |
| swap_quoted | stop | 653 | **yes** | no | yes |
| swap_quoted + one sentence | stop | 55 | **yes** | no | yes ("later as having ten thousand a year (part04.md)") |

Citation binding in the swap arm: the reply labels the inserted original as
"Source 1 (part07.md excerpt)" while the text it quotes is SOURCE 2 (`part01.md`);
the baseline labels SOURCE 2 as "part02.md". Both are wrong bindings.

## Reading

- **Selection is necessary.** The correct figure appears in 0 of 2 arms without
  the original and 2 of 2 arms with it, under both task conditions. No
  instruction change produced it; only the source did.
- **Selection is not sufficient.** With the original present, both arms still
  attribute Darcy's "ten thousand a year" (SOURCE 4) to Bingley, and the long
  arm mislabels the quoted source. These are execution errors on supplied
  evidence: entity conflation and citation binding, independent of recall.
- **Decision (per the protocol):** no selector experiment is promoted; nothing
  here measured a production ranker. Selection and execution remain independent
  work. The designated original ranked semantic 4 and fused 9 in the live run
  with 7 admitted, so a selection improvement is a real target, but it would
  not by itself make this answer correct.

## Boundaries

One question, one inserted position, greedy decoding. The attachment-preview
cutoff is an observation about the live packet, not a measured cause. No
production prompt, selection policy or model changed. Complete raw replies,
templates and the manifest are in `build/ai1/`; visible text in
`build/ai1/native-visible.json`.

## Side measurement: quotation binding across every diagnostic reply

Prompted by the two wrong labels above, a deterministic text check was written
(`tools/answer_citation_binding.py`, test `tests/test_answer_citation_binding.py`)
and run over all 43 replies with request/response pairs (`build/ai1`, `am1`,
`amt1`, `ala1`, `pc1`, `pb1`, `pr1`, `pt1`; `build/answer-citation-binding.json`).
Quoted spans of at least 25 characters are normalized (typographic quotes,
dashes, whitespace, case; ellipses split a span) and located in the packet.

| Quoted pieces | in the cited source | in a different source than cited | only in the attachment preview | in no supplied text |
|---:|---:|---:|---:|---:|
| 220 | 46 (20.9%) | 39 (17.7%) | 34 (15.5%) | 101 (45.9%) |

The first suspicion, that the model maps "Source N" to `partNN.md`, is **not**
supported: of 36 "Source N (partMM.md)" labels across runs, 35 name the document
the header actually carries. The failure is between the quotation marks: 101 of
220 pieces the model presents as verbatim quotation appear nowhere in the packet
or preview, 34 come from the attachment preview rather than a numbered source,
and of the 85 found in a numbered source 39 sit in a different source than the
one cited. This holds across greedy and sampled arms, with and without history,
and in the quote-only instruction arm (18 pieces: 2 bound, 6 in another source,
10 unsupported). Figures are after REDTEAM-019 made label matching
case-insensitive; the first measurement counted 163 uppercase `SOURCE N` labels
as absent and so understated binding (15 bound, 70 other). It is a measurement
of execution on supplied evidence, independent of recall, and it gives the
`quotation_spans` and `citation_binding` review fields of cycle step 2 a
mechanical check. Limits: substring matching after normalization; a close
paraphrase inside quotation marks counts as unsupported, which is the intended
reading of quotation marks but should be kept in mind.
