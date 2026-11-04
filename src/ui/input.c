#include "input.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"

/* raylib key codes are GLFW key codes. */
const char *glfwGetKeyName(int key, int scancode);

static bool g_scripted = false;
static uint32_t g_injected[4096];
static size_t g_inj_head = 0, g_inj_count = 0;
static char *g_private_clipboard = NULL;

void input_set_scripted(bool scripted) { g_scripted = scripted; }

bool shortcut_pressed(char letter)
{
    letter = (char)tolower((unsigned char)letter);
    int physical = KEY_A + (letter - 'a');
    if (g_scripted) return IsKeyPressed(physical) || IsKeyPressedRepeat(physical);

    /* Letters can live on any printable key in non-Latin-1 layouts, so scan
     * the whole printable range, not just KEY_A..KEY_Z. */
    for (int k = KEY_APOSTROPHE; k <= KEY_GRAVE; k++) {
        if (!IsKeyPressed(k) && !IsKeyPressedRepeat(k)) continue;
        const char *name = glfwGetKeyName(k, 0);
        if (name && name[0] && !name[1]) {
            if (tolower((unsigned char)name[0]) == letter) return true;
        } else if (k == physical) {
            /* No layout name (e.g. a non-Latin layout): fall back to the
             * physical US position, which is what macOS itself does. */
            return true;
        }
    }
    return false;
}

void input_inject_chars(const uint32_t *cps, size_t n)
{
    for (size_t i = 0; i < n && g_inj_count < sizeof(g_injected) / sizeof(g_injected[0]); i++) {
        g_injected[(g_inj_head + g_inj_count) % (sizeof(g_injected) / sizeof(g_injected[0]))] = cps[i];
        g_inj_count++;
    }
}

int input_get_char(void)
{
    if (g_inj_count) {
        int cp = (int)g_injected[g_inj_head];
        g_inj_head = (g_inj_head + 1) % (sizeof(g_injected) / sizeof(g_injected[0]));
        g_inj_count--;
        return cp;
    }
    return GetCharPressed();
}

const char *input_get_clipboard(void)
{
    return g_scripted ? g_private_clipboard : GetClipboardText();
}

void input_set_clipboard(const char *utf8)
{
    if (!g_scripted) {
        SetClipboardText(utf8);
        return;
    }
    free(g_private_clipboard);
    g_private_clipboard = utf8 ? strdup(utf8) : NULL;
}
