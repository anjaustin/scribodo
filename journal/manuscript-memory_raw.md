# Manuscript memory — scope and investigation brief

This investigation concerns GAP-003, which the user selected as P0: realistic
whole-manuscript scale and sustained-use evidence. The four other product gaps
are retained in `docs/PRODUCT-GAPS.md`, not silently dropped or promoted into this
workstream. No inference implementation, dependency installation or service
configuration change is authorized by this research step.

Scribodo already has persistent chats, immutable revisions, notes/canon and source
references in MariaDB, plus derived semantic indexing in Qdrant. Persistence is
therefore not the same problem as remembering everything in an inference prompt.
The target remains Apple Silicon with 16 GB unified memory, a pinned local BF16
generative runtime, a separate local embedder and no automatic remote fallback.
The observer must remain separate from creative chat: its findings are not
creative context unless the author explicitly chooses Discuss.

The requested repository name could identify several adapters or forks. The
investigation must establish the primary repository, examined commit, license,
source availability, supported clients and model assumptions before recommending
reuse. Repository marketing is not measured evidence of recall or scalability.

Open questions include: Does the project retain original text or only summaries?
Can compressed material be restored with stable source identities? Does its
operation depend on tool-capable models, remote providers or client hooks absent
from Scribodo? How would it handle revisions, retracted canon and project
isolation? What request, latency and memory costs appear when retrieval fails?

The desired result is a bounded, testable design recommendation and benchmark
plan. Long-lived addressable memory may be achievable; literal unlimited model
attention and guaranteed perfect recall should not be promised. Any prototype
should be evaluated against existing context behavior, including negative and
adversarial cases, before an architectural commitment.
