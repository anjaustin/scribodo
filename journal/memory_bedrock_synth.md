# Bedrock findings and next experiment

See [the evidence report](../docs/MEMORY-010-quality-bedrock.md) first.

1. **Composition/evaluation:** a 64-user-only repeated history can destroy an
   otherwise correct lookup. Production-persona controls succeed on alternating
   or combined history. V2 uses the adversarial user-only layout as its common
   default; it does not estimate ordinary native writing-session quality alone.
   Preserve that failure and add native/representative histories as separate
   frozen strata, not a replacement designed to remove the hard case.
2. **Citation contract:** a correct fact answer can still fail exact opaque-ID
   and JSON requirements. Expose short, explicit handles and do the authoritative
   handle-to-source binding in host code. Keep quotation/provenance checks strict.
3. **T1 compression contract:** exact DEV replay copies eight weather observations
   and truncates at 256 tokens; it finishes at 277 with a diagnostic larger cap.
   Grammar removal does not help. The schema permits structures larger than the
   aggregate token budget; the prompt does not reliably cause compression. Fix
   the compactness/salience contract, not the validator or blindly the allowance.

Controls reject missing rendered evidence, simple batch-size/repetition-penalty
explanations, a pure 256-token cliff, chat endpoint parsing, and a Metal-only
defect as sufficient explanations. Official Transformers agreement on these
exact cases remains untested; no universal model/runtime conclusion is claimed.

Next: preregister native-composition ablations and a revised DEV brief contract;
then use new unseen held-out cases with matched budgets. No T2, default-on,
retrospective V2 rescoring, or quality-success claim. Production code and frozen
quality fixtures were not modified during this investigation.
