/* The three palettes. See theme.h. tests/test_theme_contrast.py reads this table and checks each text colour against
 * the surfaces it is drawn on (panels, page, cards, dialogs, inputs, the selected row, the selection, a highlighted menu
 * item, the accent) (WCAG 2.1: 7:1 for the author's words and interface text, 4.5:1 for secondary text,
 * links and errors, 3:1 for placeholders and coloured labels). */
#include "ui/theme.h"

#include <string.h>

#define C(hex) { (unsigned char)((hex) >> 16), (unsigned char)(((hex) >> 8) & 0xFF), (unsigned char)((hex) & 0xFF), 0xFF }
#define CA(hex, a) { (unsigned char)((hex) >> 16), (unsigned char)(((hex) >> 8) & 0xFF), (unsigned char)((hex) & 0xFF), (a) }

/* Field order as ThemePalette in theme.h. */
static const ThemePalette k_palettes[THEME_COUNT] = {
    [THEME_DARK] = {
        .name = "dark", .label = "Dark",
        .panel = C(0x181818), .field = C(0x1B1C1F), .bg = C(0x1F1F1F), .card = C(0x222427), .input = C(0x252526),
        .surface = C(0x26272B), .card_hover = C(0x2A2D31), .hover = C(0x2A2D2E), .chip = C(0x2C3036),
        .control = C(0x313131), .active = C(0x37373D), .control_hover = C(0x3C3C3C), .highlight = C(0x3A341A),
        .scrim = CA(0x000000, 0x8C), .shadow = CA(0x000000, 0x50),
        .border = C(0x2B2B2B), .border_strong = C(0x45474D), .scrollbar = CA(0x797979, 0x66),
        .text_strong = C(0xE0E0E0), .text_prose = C(0xD4D4D4), .text = C(0xCCCCCC), .text_quote = C(0xB8B8B8),
        .caret = C(0xAEAFAD), .text_muted = C(0x909090), .text_dim = C(0x757575),
        .accent = C(0x0078D4), .on_accent = C(0xFFFFFF), .accent_soft = C(0x094470), .accent_edge = C(0x3D6FA8),
        .link = C(0x6CA8E0), .selection = C(0x1D3D5E), .ok = C(0x4EC98B), .warn = C(0xE6B450), .err = C(0xF28B82),
        .insert = C(0x8CE0A4), .insert_bg = C(0x1F3A2A), .canon_edge = C(0xB08A3E), .note_edge = C(0x5A7F5A),
        .relation = C(0xB49CE8),
    },
    [THEME_LIGHT] = {
        .name = "light", .label = "Light",
        .panel = C(0xF2F1EE), .field = C(0xFFFFFF), .bg = C(0xFBFAF8), .card = C(0xF4F3F0), .input = C(0xFFFFFF),
        .surface = C(0xFFFFFF), .card_hover = C(0xEBEAE6), .hover = C(0xE9E8E4), .chip = C(0xE7E8EB),
        .control = C(0xEDEDEB), .active = C(0xDCDCE2), .control_hover = C(0xE4E4E1), .highlight = C(0xFBEBB5),
        .scrim = CA(0x000000, 0x40), .shadow = CA(0x000000, 0x24),
        .border = C(0xE2E1DC), .border_strong = C(0xC9C8C2), .scrollbar = CA(0x000000, 0x33),
        .text_strong = C(0x111110), .text_prose = C(0x1D1D1B), .text = C(0x2B2B29), .text_quote = C(0x45443F),
        .caret = C(0x1D1D1B), .text_muted = C(0x66655F), .text_dim = C(0x86857E),
        .accent = C(0x0B63C4), .on_accent = C(0xFFFFFF), .accent_soft = C(0xCFE2F8), .accent_edge = C(0x7AA5D8),
        .link = C(0x1A5DAB), .selection = C(0xBCD6F6), .ok = C(0x1B7A4B), .warn = C(0x8A5A00), .err = C(0xB8362E),
        .insert = C(0x1B7A4B), .insert_bg = C(0xD7EFDF), .canon_edge = C(0xB08A3E), .note_edge = C(0x5A7F5A),
        .relation = C(0x6A46BE),
    },
    [THEME_PAPER] = {
        .name = "paper", .label = "Paper",
        .panel = C(0xEBE2CC), .field = C(0xFBF7EC), .bg = C(0xF5EEDC), .card = C(0xEFE7D2), .input = C(0xFBF7EC),
        .surface = C(0xFAF5E8), .card_hover = C(0xE6DCC3), .hover = C(0xE3D8BE), .chip = C(0xE3D8BE),
        .control = C(0xE8DEC6), .active = C(0xDACDAF), .control_hover = C(0xE0D4B8), .highlight = C(0xF0D98A),
        .scrim = CA(0x2A1F10, 0x40), .shadow = CA(0x2A1F10, 0x2C),
        .border = C(0xD8CBAE), .border_strong = C(0xC2B293), .scrollbar = CA(0x5B4A30, 0x40),
        .text_strong = C(0x241B12), .text_prose = C(0x33281C), .text = C(0x3C3024), .text_quote = C(0x54473A),
        .caret = C(0x33281C), .text_muted = C(0x6E5E49), .text_dim = C(0x897860),
        .accent = C(0x8C4F14), .on_accent = C(0xFFFFFF), .accent_soft = C(0xE7CFA9), .accent_edge = C(0xB98A55),
        .link = C(0x7A4410), .selection = C(0xE9D3A6), .ok = C(0x42692A), .warn = C(0x805600), .err = C(0x9C3A2A),
        .insert = C(0x42692A), .insert_bg = C(0xDCE8C8), .canon_edge = C(0x9C7A2E), .note_edge = C(0x6B8A4F),
        .relation = C(0x6E4C9A),
    },
};

const ThemePalette *g_theme = &k_palettes[THEME_DARK];

void theme_select(int which) { g_theme = &k_palettes[which >= 0 && which < THEME_COUNT ? which : THEME_DARK]; }

int theme_current(void) { return (int)(g_theme - k_palettes); }

const ThemePalette *theme_palette(int which) { return which >= 0 && which < THEME_COUNT ? &k_palettes[which] : NULL; }

static bool g_translucent;

void theme_set_translucent(bool on) { g_translucent = on; }
bool theme_translucent(void) { return g_translucent; }

/* How much of the panel colour covers the material: little enough that the sidebar reads as glass (82%, which held
 * text readable over a pure white or black desktop with the material not counted, looked opaque: user testing
 * 2026-11-16). Over a mid-grey desktop, the material not counted, interface text keeps at least 4.5:1 and everything
 * else 3:1 in every palette (tests/test_theme_contrast.py); a bright or dark extreme behind relies on the material,
 * which blurs and tints toward the palette's side, as it does for macOS's own sidebars. */
#define SIDEBAR_COVER 0.40f

/* How far a coloured label on the glass moves toward the text colour: the least that holds every status and link
 * colour at its level over a white or black desktop (0.3 left Paper's error colour at 4.34:1). */
#define GLASS_INK_MIX 0.40f

Color theme_glass_ink(Color c)
{
    if (!g_translucent) return c;
    Color t = g_theme->text;
    return (Color){ (unsigned char)(c.r + (t.r - c.r) * GLASS_INK_MIX + 0.5f), (unsigned char)(c.g + (t.g - c.g) * GLASS_INK_MIX + 0.5f),
                    (unsigned char)(c.b + (t.b - c.b) * GLASS_INK_MIX + 0.5f), c.a };
}

Color theme_sidebar(void)
{
    Color c = g_theme->panel;
    if (!g_translucent) return c;
    c.a = (unsigned char)(255.0f * SIDEBAR_COVER);
    return c;
}

int theme_by_name(const char *name)
{
    for (int i = 0; name && i < THEME_COUNT; i++)
        if (!strcmp(k_palettes[i].name, name)) return i;
    return -1;
}
