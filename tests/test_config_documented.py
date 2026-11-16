"""Every SCRIBODO_* variable the product reads has a row in a variable table.

Operator-facing variables are in docs/CONFIGURATION.md and the scripted-suite ones in docs/TESTING.md ("Test-harness
environment variables"). The product's names are the quoted SCRIBODO_* literals in src/: every one is read from the
environment, directly or through src/core/experiments.c. A row is a table line whose first cell names the variable
(`A` or `A` / `B`). The services scripts' variables are checked against their own header by test_service_defaults."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
LITERAL = re.compile(r'"(SCRIBODO_[A-Z0-9_]+)"')
ROW = re.compile(r"^\| ([^|]+) \|", re.M)


def product_names():
    names = set()
    for path in (ROOT / "src").rglob("*.[ch]"):
        names |= set(LITERAL.findall(path.read_text(encoding="utf-8", errors="replace")))
    return names


def documented(rel):
    text = (ROOT / rel).read_text(encoding="utf-8")
    names = set()
    for cell in ROW.findall(text):
        names |= set(re.findall(r"`(SCRIBODO_[A-Z0-9_]+)`", cell))
    return names


class ConfigDocumentedTests(unittest.TestCase):
    def test_every_product_variable_has_a_row(self):
        names = product_names()
        self.assertGreater(len(names), 20, "the scan found the product's variables")
        rows = documented("docs/CONFIGURATION.md") | documented("docs/TESTING.md")
        self.assertEqual(sorted(names - rows), [], "read in src/ but in no variable table")

    def test_the_row_pattern_reads_a_combined_row(self):
        self.assertIn("SCRIBODO_QDRANT_MEM_LIMIT", documented("docs/CONFIGURATION.md"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
