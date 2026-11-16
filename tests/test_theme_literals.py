"""Colours come from src/ui/theme.h by role, never as literals in a view (TECHNICAL-DEBT "Layout and shell").

Fails on a raylib or Clay colour written out in src/ui, src/app or src/main.c: a brace list of three or four 0x bytes,
a (Color){...}/(Clay_Color){...} with numbers, or raylib's named colours. Transparent (Clay_Color){ 0 } is allowed."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
FILES = [*sorted((ROOT / "src/ui").glob("*.[ch]")), *sorted((ROOT / "src/app").glob("*.[ch]")), ROOT / "src/main.c"]
HEX_TRIPLE = re.compile(r"0x[0-9A-Fa-f]{2}\s*,\s*0x[0-9A-Fa-f]{2}\s*,\s*0x[0-9A-Fa-f]{2}")
NUMERIC = re.compile(r"\((?:Clay_)?Color\)\s*\{\s*(?!0\s*\})[0-9]")
NAMED = re.compile(r"\b(?:WHITE|BLACK|LIGHTGRAY|GRAY|DARKGRAY|RED|GREEN|BLUE|YELLOW|ORANGE|SKYBLUE|RAYWHITE)\b")


class ThemeLiteralTests(unittest.TestCase):
    def test_no_colour_literals_outside_the_theme(self):
        found = []
        for path in FILES:
            if path.name in ("theme.h", "theme.c"):
                continue
            for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                code = line.split("//")[0]
                if HEX_TRIPLE.search(code) or NUMERIC.search(code) or NAMED.search(code):
                    found.append(f"{path.relative_to(ROOT)}:{n}: {line.strip()}")
        self.assertEqual(found, [])

    def test_the_patterns_catch_each_form(self):
        for bad in ("(Color){ 0x1F, 0x1F, 0x1F, 0xFF }", "Color c = { 0x8B, 0x8B, 0x8B, 0xFF };", "(Color){ 0, 0, 0, 140 }",
                    "text = WHITE;", "(Clay_Color){ 40, 40, 40, 255 }"):
            self.assertTrue(HEX_TRIPLE.search(bad) or NUMERIC.search(bad) or NAMED.search(bad), bad)
        self.assertFalse(NUMERIC.search("(Clay_Color){ 0 }"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
