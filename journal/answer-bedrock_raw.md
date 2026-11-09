# Investigation scope

The observed failure is broader than retrieving the right sentence. The latest
application run supplied all six expected factual snippets, yet some replies
ignored the question, invented relationships, reproduced document delimiters,
or continued until the reply limit. The investigation must distinguish output
quality from request delivery and must not assume the pinned model is either
incapable or correctly driven merely because earlier short checks passed.

Evidence available: frozen executables, complete generated replies, as-of source
packets, source manifests, local runtime implementation and prior correctness
checks. Missing evidence: the exact final serialized requests from that run and
the raw model stream before presentation cleanup. These missing boundaries
limit causal claims. Reconstructing a request is useful, but it must be labelled
as reconstruction rather than a capture of the original run.

Questions to resolve experimentally:

- Does the same model answer correctly from a short authoritative passage?
- Does the final wire request match what the application intends to send?
- Does adding irrelevant attachments, earlier generated answers or source
  metadata change a correct answer into a failed one?
- Does generation differ with prompt caching disabled, or on an empty slot?
- Are visible reasoning-like paragraphs native model content, a template
  mismatch, or a presentation/parser issue?
- Does a failure reproduce under deterministic decoding, independent of sampling?

Keep the model artifact, runtime flags, questions and answer keys fixed during
initial isolation. Preserve failed outputs and run model workloads serially.
No production prompt, model, inference precision or retrieval-policy changes
are justified by an untested hypothesis. Success on a minimal passage establishes
only a control; it does not establish manuscript-scale correctness.
