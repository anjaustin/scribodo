# LMM NODES — partition before selecting a solution

## Observations

1. **Transport is not usable context.** Token fit and verified hydration do not
   imply that the model can use the supplied evidence. [Bedrock diagnostics]
2. **Continuity is not fact recall.** Resume intent, rejection, uncertainty and
   unresolved work are central author-session tasks. [MEMORY-002 §§1,6]
3. **Authority is relational.** Proposal, author assertion, author decision,
   quoted fictional claim and canon are not interchangeable. [Author contract]
4. **Storage units are not meaning units.** A ratifying “yes” and its proposal
   can land in separate chunks/briefs; a negation or correction may be elsewhere.
5. **Serialization changes behavior.** Native alternating, repeated user-only
   and quoted/combined histories are materially different model inputs.
6. **Provenance is mostly host work.** SQL identity, hashes, source versions and
   admission must not depend on the model reproducing opaque identifiers.
7. **T1's implemented task drifted.** Generic fact enumeration differs from the
   planned continuity/decision/open-question aid; hard caps do not induce salience.
8. **Failure progress matters.** A first failed packet can block a contiguous
   brief frontier. Skipping it silently would instead falsify coverage.
9. **Evaluation conflates axes.** Retrieval, factual answer, citation/schema,
   author-authority fidelity, resource cost and scope coverage need separate
   measurements as well as a conjunctive release decision.
10. **Numerical fidelity is not task capability.** Verified weights/runtime are
    necessary controls, not evidence of useful multi-turn writing assistance.

## Candidate solution nodes (not conclusions)

11. **A bounded working brief** assembled from authorized originals and current
    task, with explicit source/role/order and optional noncanonical derived aids.
12. **A persistent semantic state layer** could help preserve decisions/branches,
    or could create a difficult-to-revoke second authority. It must earn its cost.
13. **Kinds of unknown differ.** An undecided narrator is pending work; a bell
    deliberately left ambiguous is a continuing creative constraint, not a TODO.
14. **Memory changes behavior, not just knowledge.** A correct card can induce
    card-writing or invention; a noncanonical stale brief can bias an answer.
15. **Optional layers need non-interference.** Brief failure should not suppress
    still-eligible original evidence; proof metadata should not consume the
    model's attention when the host already owns that proof.

## Tensions and dependencies

- N2/N3 versus N7: shorter summaries can erase the very distinctions continuity
  requires. Define the memory product before tightening its JSON schema.
- N4/N5 versus N1: reorganizing text may improve comprehension but damage
  adjacency, chronology or instructions. Test both benefits and semantic loss.
- N6 versus N12: host authority is deterministic; semantic extraction is not.
  Do not use a generated state label as an authorization/canon boundary.
- N8 versus usefulness: retain a truthful coverage frontier without making one
  difficult source an invisible permanent barrier to all continuity.
- N10 constrains N11/N12: establish an achievable model/task envelope before
  investing in a hierarchy or knowledge graph.

Complexity check: these tensions do not collapse to a single prompt fix. Continue
through REFLECT and adversarial TEST/MODEL/REFINE cycles.
