# COMPOSITION-001 C02 — revision identity boundary

**Date:** 2026-11-25. **Baseline:** `50286c2`.
**State:** design and red contract probe; no production format change or data migration.
**Prototype review:** [REDTEAM-026](REDTEAM-026-revision-identity-prototype.md); original counts below are
historical, with the remediated control and negative-control results recorded separately.
**Plan:** [COMPOSITION-001](COMPOSITION-001-plan.md).
**Evidence:** [C02 probe](../journal/composition001/c02/identity_probe.c),
[direct-run log](../journal/composition001/c02/red-probe-direct.log).

## What must change

The file backend identifies a document's revision directory by a sanitized, at-most-199-byte basename.
Distinct valid names can therefore share not only a History list but also the path used for source
snapshots. The authoritative manuscript files are separate; their distinct names do not prevent this.
F2 remains open until the write, read, rename, set-aside and import/export protocols agree on identity.

The repair must not be:

- A larger truncation buffer without a complete compatibility/identity rule.
- A hash used as authority without validating its full-name binding.
- A writer-only path change that leaves importer and rename callers guessing.
- A scan that assigns old snapshots by matching the current manuscript text. Equal content is not
  ownership, and edits/deletions make current files incomplete evidence.
- A bulk rewrite of old histories or source records. Original names, bytes and snapshot references remain
  evidence; irrecoverably mixed histories require explicit author recovery, not guessed partitioning.

## Source-grounded consumer map

| Boundary | Current behavior | Required coordinated change |
|---|---|---|
| `src/app/revisions.c`: `doc_dir`, `revision_lock` | Sanitizes/truncates, creates/opens a per-directory lock; each operation later recomputes the path | One checked resolver shared by lock and actual I/O; no truncated `snprintf` success or different identity between locking and reading |
| `revisions.c`: list/record/snapshot/load | Lists an index and loads sibling snapshot files; text hash checks integrity, not document ownership | Validate document ownership before returning bytes, including when a source holds only the snapshot filename and no hash |
| `src/app/document_workflow.c`: `set_aside_history` | Constructs a literal basename path and renames to a timestamp suffix | Use the same resolver/identity operations; preserve the older generation rather than let a reused name inherit it |
| `document_workflow.c`: `finish_prompt` | Renames the manuscript first, then moves History and appends the rename log; some History failures are reported after the file rename | Preflight identity ambiguity before renaming the manuscript; define interruption/recovery and rollback without silently separating text from history |
| `src/app/store_import.c`: `import_revisions` | Enumerates directories, treats each directory name as the document name, derives DB revision IDs from project/name/snapshot | Enumerate logical full names and their verified history location; reject unresolved old ownership before adopting the project; preserve ID derivation for unchanged legacy names |
| `store_import.c`: `source_revision` | Finds a matching snapshot and resolves source basename through dated rename records | Match the verified document identity as well as the snapshot; never attach by matching filename alone |
| `store_import.c`: `export_files_unlocked` | Sends every SQL document/revision through the file APIs; maps DB IDs to the resulting snapshot filenames | Prove disjoint export of long/unsafe names, rename generations and sources; round-trip through a fresh throwaway DB |
| `src/app/record_workflow.c`: source creation/navigation | `SourceRef` stores `doc[256]` and `revision[160]`; navigation parses the timestamp at the beginning of the snapshot name and follows dated renames | Keep old tokens interpretable. If adding new tokens, preserve the leading timestamp contract or update both consumers and tests together |
| `src/app/project_store.c` | File operations run under a shared project operation lock; DB and paused adapters are distinct | Do not upgrade a shared project lock in place or silently switch authority. Specify any new name-map lock order relative to project/history locks |
| `src/app/renames.c` | Applies rename records in order and only since a source's creation time | Preserve name-reuse semantics; history identity is not just the currently resolved filename |
| `tests/test_llm.c`, `test_persistence_faults.c`, UI/DB tests | Many negative tests deliberately modify literal legacy history paths | Keep legacy-format fixture tests; add new-format fixtures rather than merely changing every test to the new path |

Other constraints discovered:

- The New/Rename prompt admits names **up to 200 bytes** (`doc_name`), not 199. Imported or external
  documents can be longer. Reducing only the prompt limit does not fix existing histories or external files.
- `Revision.file` and `SourceRef.revision` are each 160 bytes. An encoding must not silently truncate their
  identities or break the leading timestamp parsed for rename resolution.
- A revision can be valid even if its manuscript was deleted. Import cannot restrict History enumeration
  to files still present in the project.
- A readable old history may be read-only and lack a `.lock` file; the existing read-only compatibility
  test must continue passing without creating a new marker as a prerequisite for reading.
- Case/normalization equivalence is a filesystem fact. Do not normalize original basename bytes or rely
  on `strcasecmp` to decide whether two actual documents are one file.

## Implementation decision

**First implement and test the identity resolver outside the default write route. Do not change the
production writer until rename/set-aside and import enumeration can use the same identity contract.**
This is a temporary development boundary, not a permanent runtime backend flag or another shipped store.

Evaluate two small representations in the isolated C02 harness:

1. **Full-byte reversible keys in a versioned namespace.** Directory components encode the complete
   basename, with explicit length and bounded components. No hash collision or name metadata is needed,
   but long-name directory depth, enumeration, archived generations and move semantics require tests.
2. **Bounded digest key plus exact owner metadata.** A versioned directory uses an existing native SHA-256
   primitive; the full original name and format version are stored and checked, not inferred from the digest.
   This is shallower but adds a durable ownership publication/recovery boundary, especially across renames.

The selection gate is not speed or aesthetics: choose the representation with the smaller complete,
tested rename/archive/import/recovery protocol. No new crypto package, Unicode normalizer, global
framework or SQL-schema rewrite is justified by this defect. Reusing an existing native hash is allowed,
but does not by itself resolve ownership or crash consistency.

Ordinary legacy directories should stay readable in place. They may be reused only when their ownership
is unambiguous under the documented compatibility rules. New-format directory names must be disjoint
from the old namespace; there must be no “if new lookup fails, try the sanitized name” fallback.

### Legacy ambiguity contract

The old index records snapshots, origin, hash, timestamp and size—not the exact owning basename. Where
the mapping lost bytes, the missing information cannot be reconstructed reliably. In particular:

- A shared 199-byte prefix directory must not be assigned to either longer document merely because only
  one currently exists, matches a quote, or happens to be imported first.
- For sanitization aliases, coexistence of distinct candidate names is a known ambiguity, not permission
  to merge or choose. Absence of a competing current file is not proof about deleted historical owners.
- Fail the **checked** History/source operation with outputs cleared. Do not turn ambiguity into an empty
  successful History list, silently create a new authority beside the mixed one, or update source bindings.
- Preserve the original directory untouched. Present a specific recoverable error and keep manuscript
  editing available under the existing save/failure contract. Any author-assisted reassignment belongs to
  a separate explicit recovery operation; it is not authorized by the C02 prototype.
- Import/export must not mark a project adopted or a fork snapshot ready after encountering unresolved
  ownership. Test the actual transaction/ready-marker behavior, not only the codec.

**Open design constraint to resolve before production changes:** the exact legacy eligibility rule must
not claim ownership evidence the old format does not contain. Overly broad refusal of all underscore
histories would also be an unjustified compatibility regression. The isolated representation test must
make this tradeoff explicit and retain ambiguous originals, rather than hide it in a fallback branch.

## Ordered implementation/test steps

1. Keep the retained contract probe red on `50286c2`; its ordinary short-name control and read-only legacy
   compatibility are controls, not targets to weaken. Promote proved invariants into the existing suites
   when the repair is ready.
2. Prototype the selected resolver/key/owner parser, including corruption, component bounds, full-buffer
   rejection, namespace collisions and missing/partial metadata. Test archive/rename paths before selection.
3. Centralize all file-history path decisions and identity enumeration. Preserve existing snapshot tokens
   for legacy histories; use explicit new-format identity where required. Test exact source hydration.
4. Integrate rename/set-aside with preflight and interruption recovery. Cover short↔long, long↔long,
   case-only and normalization-only rename, destination history, name reuse, deleted documents and
   source references created before each rename. Distinguish real volume tests from simulated case behavior.
5. Integrate import/export and fork recovery on synthetic projects. Verify original bytes, identities,
   ordered revisions, old/new sources and no partial adoption after corruption/ambiguity.
6. Red-team, then run `make test`, `make analyze`, `make sanitize`, app build and affected UI gates.
   Run full test/sanitizer serially; DB/crash/DB-UI gates also serially on an isolated store. No model request
   changes are planned; no real-model quality claim follows from these mechanical gates.

## Current evidence and limits

The C02 probe makes new temporary synthetic projects, uses public revision APIs, and retains the fixtures
for inspection. It never connects to a database or calls a model. It covers:

- New distinct names sharing the first 199 bytes.
- New colon/underscore sanitization aliases.
- Ordinary short names as a successful isolation control.
- An ordinary pre-lock legacy index as a compatibility control.
- An already ambiguous legacy directory: checked list/load/record must refuse without inventing ownership.

On `50286c2`, the probe executes **52 checks, 12 failures**, exiting **1**. The Make overlay reports **2**
because its recipe returned 1. That is an intentional red regression result, not a successful repair or a
crash. Exact commands are in the [progress journal](../journal/composition001-progress.md).
The normal test targets are unchanged; no passing test was weakened to accommodate this probe.

Rename/import/export and crash recovery tests are requirements, **not yet executed C02 evidence**.
F2 remains open. No production code, file format, dependency, author data or database was changed.

### Native key experiment

The retained [key prototype](../journal/composition001/c02/key_probe.c) compiles as a standalone C11
executable with ASan/UBSan, strict warnings and the existing macOS CommonCrypto primitive. It is not linked
into Scribodo. [Execution evidence](../journal/composition001/c02/key-test.log): **71,648 checks, zero
failures**, compile/run exit **0**.

Candidate A encodes names as `.names-v1/<three-digit-byte-length>/<lowercase-hex-components>`, with at most
120 hex characters per component. Length separates prefix names, so a document leaf is never a parent of
another document's leaf. The prototype round-trips every length 1–255, rejects every undersized output
without exposing a truncated key, rejects malformed encodings, and preserves case, composed/decomposed
Unicode and colon/underscore distinctions byte-for-byte. Synthetic directory moves preserve sibling
snapshots and exercise a separate archive namespace without deleting content.

Candidate B computes a SHA-256 key and checks the exact full-name owner. Tests reject an incorrect,
truncated or mismatched owner. This tests the binding predicate, **not durable owner publication or rename
recovery**. A digest path is 74 bytes versus A's maximum relative path of 528 bytes. These are path lengths,
not runtime-memory or latency measurements. Native source-path bounds must reject a project+key path that
does not fit rather than truncate it; A's longer key cannot be ignored merely because typical projects fit.

**Working preference:** A, because it removes a separate owner-file commit and digest-binding failure mode.
Do not promote that preference yet: the prototype's rename/archive moves test directory topology only.
It does not implement the app's source-name journal, logical archive generations, imported IDs, read-only
legacy discovery or symlink-safe resolver. Those are the next selection gates. No production identity
guarantee follows from a large count of pure codec assertions.

### REDTEAM-026 follow-up

The native key harness now rejects directory symlinks/type mismatches and truncated output paths, and
checks exact snapshot bytes rather than existence. The final repaired ASan/UBSan control passes **101,676
assertions**, including seeded arbitrary-byte/corrupted-key properties; all five deliberately weakened
controls fail with assertion exit 1. The public-API probe distinguishes setup/control errors (exit 2)
from expected unresolved product failures (exit 1). Its F2 result remains **52 checks, 12 failures,
zero setup/control errors**. See the review/journal for commands and repository-gate outcomes.
