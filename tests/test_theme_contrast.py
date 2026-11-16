"""Every palette in src/ui/theme.c is legible (GAP-005): each text colour against each surface it is drawn on.

WCAG 2.1 contrast: 7:1 (AAA) for the author's words and interface text; 4.5:1 (AA) for secondary text, quotes, links
and errors; 3:1 for placeholders and coloured labels (findings' kinds, ok and warning marks), and for text on the
accent at 4.5:1 (a primary button darkens under the pointer, never lightens, so that holds there too). The surfaces
are the ones text sits on: the panels, the page, cards, dialogs, inputs, the selected row, the selection and a
highlighted menu item. Dimmed paragraphs are faint by design and not held to these."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SURFACES = ("panel", "bg", "card", "surface", "input", "field")
REQUIRED = {"text_prose": 7, "text": 7, "text_strong": 7, "text_quote": 4.5, "text_muted": 4.5, "text_dim": 3,
            "link": 4.5, "err": 4.5, "ok": 3, "warn": 3, "insert": 3, "relation": 3}


def palettes():
    src = (ROOT / "src/ui/theme.c").read_text(encoding="utf-8")
    out = {}
    for name, body in re.findall(r'\n    \[THEME_(\w+)\]\s*=\s*\{(.*?)\n    \},', src, re.S):
        out[name.lower()] = {k: v for k, v in re.findall(r"\.(\w+)\s*=\s*CA?\(0x([0-9A-Fa-f]{6})", body)}
    return out


def luminance(hex6):
    def ch(x):
        x = int(x, 16) / 255
        return x / 12.92 if x <= 0.03928 else ((x + 0.055) / 1.055) ** 2.4
    r, g, b = (ch(hex6[i:i + 2]) for i in (0, 2, 4))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


class ThemeContrastTests(unittest.TestCase):
    def test_three_palettes_with_every_role(self):
        p = palettes()
        self.assertEqual(sorted(p), ["dark", "light", "paper"])
        roles = set(p["dark"])
        for name, pal in p.items():
            self.assertEqual(set(pal), roles, name)

    def test_text_is_legible_on_every_surface(self):
        failures = []
        for name, pal in palettes().items():
            for fg, need in REQUIRED.items():
                for bg in SURFACES:
                    r = contrast(pal[fg], pal[bg])
                    if r < need:
                        failures.append(f"{name}: {fg} on {bg} {r:.2f} < {need}")
            for fg in ("text", "text_strong"):
                if contrast(pal[fg], pal["active"]) < 7:
                    failures.append(f"{name}: {fg} on the selected row")
            if contrast(pal["on_accent"], pal["accent"]) < 4.5:
                failures.append(f"{name}: text on the accent")
            # Selected text is still the author's words; a highlighted menu item is still a label (red team).
            if contrast(pal["text_prose"], pal["selection"]) < 7:
                failures.append(f"{name}: prose on the selection {contrast(pal['text_prose'], pal['selection']):.2f}")
            if contrast(pal["text_strong"], pal["accent_soft"]) < 7:
                failures.append(f"{name}: a highlighted menu item")
        self.assertEqual(failures, [])

    def test_the_check_would_catch_a_bad_pair(self):
        self.assertLess(contrast("777777", "666666"), 3)
        self.assertAlmostEqual(contrast("FFFFFF", "000000"), 21, places=1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
