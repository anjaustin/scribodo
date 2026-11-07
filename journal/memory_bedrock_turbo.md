# Controlled diagnostic checks

Model input -> template -> tokenized prompt -> generated content -> structured
answer -> citation validation -> quality score. Test each edge independently.

## Completed controls

- The original fresh minimal fact lookup succeeds before and after the history
  tests: persistent slot corruption is not required to reproduce the failure.
- The fact and question are present in every rendered case examined.
- The two-prior-user-turn strict-JSON case still abstains with batch 64, 256 or
  512, and with repetition penalty 1.0 instead of inherited 1.1. This damages
  the initial simple batch-boundary/repetition-penalty explanation.
- The combined-text case exceeds 256 tokens and still answers the fact; crossing
  256 prompt tokens alone is not sufficient to explain the failure.
- Raw `/completion` fed the rendered token IDs reproduces the chat answers for
  the single, two-user and combined cases. HTTP chat-response parsing is not
  required to reproduce the failure.
- Two prior turns with a plain question still yield 47; role layout alone is not
  sufficient either. The long policy/format instruction and composition interact.
- The unchanged production brief prompt/schema succeeds on one fact, four facts,
  and four facts with narrative filler: 35, 94 and 90 completion tokens. The
  schema is not universally incompatible, and 256 tokens is not universally too
  short. Replay failed DEV inputs before prescribing any schema/budget change.

## Important separation

Factual correctness is not citation correctness. A successful fact answer can
still invent an ID, copy the entire source header as its ID, or use a numeric
answer instead of the required string. Keep the frozen score unchanged and
report these diagnostic dimensions separately.
