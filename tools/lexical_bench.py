#!/usr/bin/env python3
"""Measure the lexical discovery channel in isolation (MEMORY-015 S3).

Loads a frozen reach fixture's pre-boundary archive into a scratch table with
the production full-text index (InnoDB, utf8mb4_bin), runs every task's
question under the production query mode and candidate alternatives, and
reports where the required sources rank in each. No planner, no model, no
vectors: this isolates what MATCH ... AGAINST returns. Uses the running
MariaDB container through tools/services.sh's throwaway-database command.
Only the scratch database is touched and it is dropped afterwards.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import memory_fixture  # noqa: E402

LIMIT = 16


def sql_string(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'").replace("\n", "\\n") + "'"


def run_sql(database, sql):
    cmd = ["docker", "exec", "-i", "scribodo-mariadb-1", "sh", "-c",
           f'MYSQL_PWD="$(cat /run/secrets/db_root_password)" mariadb -uroot --batch --raw --skip-column-names {database}']
    result = subprocess.run(cmd, input=sql, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return [line.split("\t") for line in result.stdout.splitlines() if line]


def boolean_terms(question):
    text = unicodedata.normalize("NFKC", question)
    words = re.split(r"[^\w]+", text)
    return " ".join(w for w in words if len(w) >= 3)


def variants(question):
    q = sql_string(question)
    b = sql_string(boolean_terms(question))
    nat = f"MATCH(text) AGAINST({q} IN NATURAL LANGUAGE MODE)"
    boo = f"MATCH(text) AGAINST({b} IN BOOLEAN MODE)"
    return {
        "natural (production)": (nat, nat),
        "natural / log(len)": (f"{nat} / LOG(2 + CHAR_LENGTH(text))", nat),
        "natural / sqrt(len)": (f"{nat} / SQRT(CHAR_LENGTH(text))", nat),
        "natural / len^0.6": (f"{nat} / POW(CHAR_LENGTH(text), 0.6)", nat),
        "natural / len^0.75": (f"{nat} / POW(CHAR_LENGTH(text), 0.75)", nat),
        "natural / len^0.9": (f"{nat} / POW(CHAR_LENGTH(text), 0.9)", nat),
        "natural / len": (f"{nat} / CHAR_LENGTH(text)", nat),
        "boolean OR": (boo, boo),
        "boolean OR / log(len)": (f"{boo} / LOG(2 + CHAR_LENGTH(text))", boo),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seed", type=int, default=20261107)
    parser.add_argument("--words", type=int, default=30000)
    parser.add_argument("--per-category", type=int, default=3)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--collation", default="utf8mb4_bin", help="the text column collation (production: utf8mb4_bin)")
    args = parser.parse_args()
    fixture = memory_fixture.build(args.seed, args.words, args.per_category)
    database = f"scribodo_test_lexbench_{os.getpid()}"
    subprocess.run([str(ROOT / "tools/services.sh"), "testdb", "create", database], check=True, capture_output=True)
    try:
        run_sql(database, "CREATE TABLE units (seq INT PRIMARY KEY, role VARCHAR(16) NOT NULL, text VARCHAR(2000) NOT NULL,"
                          f" FULLTEXT KEY ft (text)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE={args.collation};")
        rows = [r for r in fixture["archive"] if r["sequence"] < fixture["boundary"]]
        for i in range(0, len(rows), 100):
            values = ",".join(f"({r['sequence']},{sql_string(r['role'])},{sql_string(r['text'])})" for r in rows[i:i + 100])
            run_sql(database, f"INSERT INTO units (seq, role, text) VALUES {values};")
        # No OPTIMIZE TABLE: a rebuilt InnoDB full-text index on a _bin column
        # lowercases tokens while incrementally inserted rows keep their case,
        # which made capitalized names unsearchable in an earlier run of this
        # bench (MEMORY-018). Production inserts incrementally; so does this.
        report = {"fixture": {"seed": args.seed, "words": fixture["words"], "rows": len(rows)}, "variants": {}}
        tasks = [t for t in fixture["tasks"] if t["required_sequences"]]
        names = list(variants("x").keys())
        for name in names:
            per_cat = {}
            ranks = []
            for task in tasks:
                score_expr, match_expr = variants(task["question"])[name]
                got = run_sql(database, f"SELECT seq FROM units WHERE {match_expr} > 0 ORDER BY ({score_expr}) DESC, seq LIMIT {LIMIT};")
                top = [int(r[0]) for r in got]
                required = task["required_sequences"]
                hit = sum(1 for s in required if s in top) / len(required)
                rank = [top.index(s) + 1 if s in top else None for s in required]
                cell = per_cat.setdefault(task["category"], {"n": 0, "hit": 0.0, "ranks": []})
                cell["n"] += 1
                cell["hit"] += hit
                cell["ranks"].extend(rank)
                ranks.append((task["id"], task["category"], rank))
            for cell in per_cat.values():
                cell["hit"] = round(cell["hit"] / cell["n"], 3)
            overall = sum(c["hit"] * c["n"] for c in per_cat.values()) / sum(c["n"] for c in per_cat.values())
            found = [r for _, _, rs in ranks for r in rs if r is not None]
            report["variants"][name] = {"overall_hit": round(overall, 4), "per_category": per_cat,
                                        "worst_found_rank": max(found) if found else None,
                                        "mean_found_rank": round(sum(found) / len(found), 2) if found else None,
                                        "not_found": sum(1 for _, _, rs in ranks for r in rs if r is None)}
        # Print a compact table.
        cats = sorted({t["category"] for t in tasks})
        print(f"fixture words={fixture['words']} rows={len(rows)} tasks={len(tasks)}")
        print(f"{'variant':24} {'overall':>7} " + " ".join(f"{c[:9]:>9}" for c in cats))
        for name, v in report["variants"].items():
            print(f"{name:24} {v['overall_hit']:>7.3f} " + " ".join(f"{v['per_category'].get(c, {}).get('hit', 0):>9.2f}" for c in cats) + f"   worst_rank={v['worst_found_rank']} mean_rank={v['mean_found_rank']} not_found={v['not_found']}")
        if args.json:
            args.json.write_text(json.dumps(report, indent=2))
    finally:
        subprocess.run([str(ROOT / "tools/services.sh"), "testdb", "drop", database], check=False, capture_output=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
