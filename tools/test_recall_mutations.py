#!/usr/bin/env python3
"""Mutation checks for the deterministic recall path (MEMORY-015 S1).

Each case weakens one host-side guard in src/app/recall.c that SQL does not
also enforce, relinks test_recall against the project's existing objects and
expects the suite to fail. Guards that SQL enforces independently (consent,
project isolation, source-hash freshness) are deliberately not listed: their
host checks are defense in depth and a surviving mutant there would be noise,
not a gap. Needs the local services (tools/services.sh up) and a prior build
of build/test_recall. Only files under build/recall-mutations change.
"""
import os
from pathlib import Path
import subprocess


FRAGMENT = """
RECALL_MUTANT_OBJS := $(filter-out $(BUILD)/$(ORIGINAL_OBJ),$(CORE_OBJS) $(LLM_OBJS)) $(BUILD)/tests/test_recall.o
.PHONY: recall-mutant
recall-mutant: $(RECALL_MUTANT_OBJS)
\t$(CC) $(CFLAGS) $(INCLUDES) -Isrc/app $(MUTANT) $(RECALL_MUTANT_OBJS) -o $(OUT) $(MARIADB_LIBS) -lpthread
"""


def main():
    root = Path(__file__).resolve().parents[1]
    output = root / "build/recall-mutations"
    output.mkdir(parents=True, exist_ok=True)
    cases = [
        ("control", None),
        ("status_filter", ('if (strcmp(v->status, "committed") != 0) {', "if (false) {")),
        ("canon_term_match", ("        if (!matched) continue;\n        RecallRow *row;\n        ok = push_row(r, capacity, &row);\n        if (!ok) break;\n        row->kind = RECALL_CANON;",
                              "        RecallRow *row;\n        ok = push_row(r, capacity, &row);\n        if (!ok) break;\n        row->kind = RECALL_CANON;")),
        ("canon_rank", ("    if (x->matched != y->matched) return x->matched > y->matched ? -1 : 1;\n    if (x->ts != y->ts)",
                        "    if (x->ts != y->ts)")),
        ("canon_cap", ("if (canon_count > RECALL_CANON_ROWS_MAX) {", "if (false) {")),
        ("no_terms", ("    if (!out->terms) {\n        out->canon_ok = true;", "    if (false) {\n        out->canon_ok = true;")),
        ("query_bound", ("if (strnlen(query, RECALL_QUERY_BYTES + 1) > RECALL_QUERY_BYTES) {", "if (false) {")),
        # Store-side guard the recall path depends on: lexical length normalization (MEMORY-018).
        ("lexical_length_norm", ('" / POW(CHAR_LENGTH(u.text)," MEMORY_LEXICAL_LENGTH_NORM ") AS score"', '" AS score"'), "src/app/memory_store.c"),
        ("lexical_case_fold", ("COLLATE utf8mb4_unicode_ci NOT NULL", "COLLATE utf8mb4_bin NOT NULL"), "src/app/db.c"),
        ("short_token_fallback", ("#define RECALL_FULLTEXT_MIN_TOKEN 3u", "#define RECALL_FULLTEXT_MIN_TOKEN 0u")),
    ]
    for case in cases:
        name, replacement = case[0], case[1]
        source_path = case[2] if len(case) > 2 else "src/app/recall.c"
        text = (root / source_path).read_text()
        if replacement:
            if replacement[0] not in text:
                raise RuntimeError(f"Mutation no longer matches: {name}")
            text = text.replace(*replacement, 1)
        mutant = output / f"{name}.c"
        mutant.write_text(text)
        binary = output / name
        with (output / f"{name}.log").open("w") as log:
            original_obj = source_path.replace(".c", ".o")
            subprocess.run(["make", "--no-print-directory", "-f", "Makefile", "-f", "-", "recall-mutant",
                            f"MUTANT={mutant}", f"OUT={binary}", f"ORIGINAL_OBJ={original_obj}"], cwd=root, input=FRAGMENT, text=True,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
            result = subprocess.run([str(binary)], cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=600)
        if replacement is None:
            if result.returncode:
                raise RuntimeError(f"Unmodified control failed: {name}")
            print(f"PASS {name}", flush=True)
        else:
            if not result.returncode:
                raise RuntimeError(f"Mutation survived: {name}")
            print(f"KILLED {name}", flush=True)


if __name__ == "__main__":
    main()
