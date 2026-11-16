# Bundled fonts

The default interface, document and monospace faces: Google's **Share** family
by Carrois Type Design, under the SIL Open Font License 1.1 (the `OFL-*.txt`
files beside them). Authors can choose any installed font per role instead
(the app's **Appearance** panel; [USER-GUIDE](../../docs/USER-GUIDE.md#appearance)).

Fetched from [google/fonts](https://github.com/google/fonts) at commit
`7af0d90b4d532ffa4b041937b560c6df4e7b1e95`, unmodified.
`tests/test_bundled_fonts.py` pins each file's SHA-256.

| File | Upstream path | Default role | SHA-256 |
|---|---|---|---|
| `Share-Regular.ttf` | `ofl/share/Share-Regular.ttf` | Document and chat text | `ef1c3522c12628f19174fbdb2939a2dbd03c83d2498e4a8c0c74c6a4caa9914b` |
| `ShareTech-Regular.ttf` | `ofl/sharetech/ShareTech-Regular.ttf` | Interface | `2d1dc42631c22ce99285a3a7c1d0ca9e0bae9e67aa1ca01a501081f27a203df4` |
| `ShareTechMono-Regular.ttf` | `ofl/sharetechmono/ShareTechMono-Regular.ttf` | Monospace (status bar, figures) | `9ceab1f87414829af259c0f537573ae03ef7dd3147c0b27a36a1a0beb6732677` |
| `OFL-Share.txt` | `ofl/share/OFL.txt` | Licence | |
| `OFL-ShareTech.txt` | `ofl/sharetech/OFL.txt` | Licence | |
| `OFL-ShareTechMono.txt` | `ofl/sharetechmono/OFL.txt` | Licence | |

**Coverage.** Each face holds about 250 glyphs: Basic Latin, Latin-1 and the
common typographic punctuation. Letters outside it (ř, č, ą, ń, Greek,
Cyrillic) and interface symbols (⌘, ⇧, ●) come from a fallback face, scaled
to the primary face's cap height and set on its baseline (`src/ui/font.c`).

"Share" is a Reserved Font Name: a modified version must not be distributed
under that name (OFL §3).
