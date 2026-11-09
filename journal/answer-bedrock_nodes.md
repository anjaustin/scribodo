# Observable boundaries and tests

1. Saved source: byte-verified manuscript and as-of source packets.
2. Host construction: optional document heads, retained history, recalled sources.
3. Wire payload: final JSON after sampling and memory insertion; needs capture.
4. Template: rendered roles, generation suffix, reasoning configuration.
5. Runtime state: slot cache, recurrent model state, reused versus cold prompts.
6. Decoding: fixed seed versus deterministic decoding; length-limit behavior.
7. Presentation: raw stream versus visible text and persisted reply.
8. Evaluation: all clauses and quotations versus keyword-only checks.

Dependencies: test raw inference before attributing failures to presentation;
capture requests before asserting that an intended field reached inference.
Tensions: correct source delivery does not establish usable context; a good
minimal answer does not establish a correct long-context runtime.

Initial order: minimal grounded controls; exact request capture/replay; controlled
component removal and cache comparisons; only then select a remediation.
