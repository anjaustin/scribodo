"""The bundled Share fonts are the pinned upstream files, with their licences (assets/fonts/README.md)."""
import hashlib
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "assets/fonts"
PINNED = {
    "Share-Regular.ttf": "ef1c3522c12628f19174fbdb2939a2dbd03c83d2498e4a8c0c74c6a4caa9914b",
    "ShareTech-Regular.ttf": "2d1dc42631c22ce99285a3a7c1d0ca9e0bae9e67aa1ca01a501081f27a203df4",
    "ShareTechMono-Regular.ttf": "9ceab1f87414829af259c0f537573ae03ef7dd3147c0b27a36a1a0beb6732677",
}


class BundledFontTests(unittest.TestCase):
    def test_each_font_is_the_pinned_file(self):
        for name, sha in PINNED.items():
            self.assertEqual(hashlib.sha256((FONTS / name).read_bytes()).hexdigest(), sha, name)

    def test_the_readme_states_the_same_hashes(self):
        readme = (FONTS / "README.md").read_text(encoding="utf-8")
        for name, sha in PINNED.items():
            self.assertIn(f"`{name}`", readme)
            self.assertIn(sha, readme)

    def test_every_font_ships_with_its_licence(self):
        for lic in ("OFL-Share.txt", "OFL-ShareTech.txt", "OFL-ShareTechMono.txt"):
            text = (FONTS / lic).read_text(encoding="utf-8")
            self.assertIn("SIL Open Font License, Version 1.1", text, lic)

    def test_nothing_unlisted_is_bundled(self):
        listed = set(PINNED) | {"OFL-Share.txt", "OFL-ShareTech.txt", "OFL-ShareTechMono.txt", "README.md"}
        self.assertEqual(sorted(p.name for p in FONTS.iterdir()), sorted(listed))


if __name__ == "__main__":
    unittest.main(verbosity=2)
