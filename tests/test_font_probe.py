"""The font probe (ui/font.h): `scribodo --font-probe FILE PX` rasterises a face as the app would and exits 0, and a
malformed face that raylib's loader cannot survive kills the probe, not the app.

The malformed face is the red team's (2026-11-16): Share-Regular with head.indexToLocFormat flipped from 0 to 1, which
every check in core/font_file accepts and stb_truetype reads past the end of. Needs build/scribodo (make)."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "build/scribodo"
SHARE = ROOT / "assets/fonts/Share-Regular.ttf"


def table(data, tag):
    n = struct.unpack(">H", data[4:6])[0]
    for i in range(n):
        t, _, off, length = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        if t == tag:
            return off, length
    raise KeyError(tag)


def probe(path, px=24):
    return subprocess.run([str(APP), "--font-probe", str(path), str(px)], capture_output=True, timeout=30).returncode


class FontProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not APP.exists():
            raise unittest.SkipTest("build/scribodo is not built")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def write(self, name, data):
        p = self.dir / name
        p.write_bytes(data)
        return p

    def test_the_bundled_faces_pass(self):
        for name in ("Share-Regular.ttf", "ShareTech-Regular.ttf", "ShareTechMono-Regular.ttf"):
            self.assertEqual(probe(ROOT / "assets/fonts" / name), 0, name)

    def test_a_wrong_loca_format_fails_the_probe(self):
        data = bytearray(SHARE.read_bytes())
        head, _ = table(data, b"head")
        self.assertEqual(struct.unpack(">h", data[head + 50:head + 52])[0], 0)
        data[head + 50:head + 52] = struct.pack(">h", 1)
        self.assertNotEqual(probe(self.write("bad-loca.ttf", bytes(data))), 0)

    def test_scrambled_glyph_data_fails_or_passes_but_never_hangs(self):
        data = bytearray(SHARE.read_bytes())
        glyf, length = table(data, b"glyf")
        for i in range(glyf, glyf + length, 7):
            data[i] = 0xFF
        rc = probe(self.write("scrambled.ttf", bytes(data)))  # the timeout above is the check
        self.assertIsInstance(rc, int)

    def test_not_a_font_and_bad_arguments_fail(self):
        self.assertNotEqual(probe(self.write("text.ttf", b"plain text, not a font")), 0)
        self.assertNotEqual(probe(self.dir / "missing.ttf"), 0)
        self.assertNotEqual(probe(SHARE, px=0), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
