# Research provenance and local source caches

These four authored research notes and the [report](../../reports/How%20billion%20context%20works.md)
inspect `ranxianglei/billion-context` at immutable commit
`c3dfbe6b21a1b2fc3642594043bc81fd2ae91247`. Code/document citations link directly
to that commit. Inspected upstream regression tests are not claimed as executed
or passing; benchmark results are reported, not reproduced.

During research, read-only downloads were retained locally in `artifacts/`,
`evidence_sources/`, `kernel_source/` and `runtime_source/`. Those cache paths are
intentionally ignored by Git. References to their local filenames in the notes
describe the research workspace, not required checked-in deliverables. This
avoids vendoring roughly 118 MB of duplicated upstream material and introducing
an unreviewed dependency tree. Existing caches have not been deleted or executed.

To obtain the same primary source without running its code:

```sh
mkdir -p build/research/billion-context
curl -fL https://github.com/ranxianglei/billion-context/archive/c3dfbe6b21a1b2fc3642594043bc81fd2ae91247.tar.gz -o build/research/billion-context/source.tar.gz
tar -xzf build/research/billion-context/source.tar.gz -C build/research/billion-context
```

The immutable Git commit, rather than archive-container bytes, is the source
identity. No upstream installation, build, tests or model calls are needed for
source inspection. Upstream's root/kernel license texts and provenance should be
reviewed at that pinned tree before any code reuse; Scribodo's native foundation
does not copy upstream implementation code.

Issue #5796 is mutable external evidence, cited as inspected on 2026-11-06, not
as commit-pinned code or a reproduced failure. The LMM method is separately
pinned to `anjaustin/lmm@55719c0a2308d1a3f29ca656ed7b6287be33bca2` in the MVP
design and phase records.
