#include "keys.h"

#include <ctype.h>

#include "raylib.h"

/* raylib key codes are GLFW key codes; GLFWwindow is not needed here. */
const char *glfwGetKeyName(int key, int scancode);

bool shortcut_pressed(char letter)
{
    letter = (char)tolower((unsigned char)letter);
    /* Letters can live on any printable key in non-Latin-1 layouts, so scan
     * the whole printable range, not just KEY_A..KEY_Z. */
    for (int k = KEY_APOSTROPHE; k <= KEY_GRAVE; k++) {
        if (!IsKeyPressed(k) && !IsKeyPressedRepeat(k)) continue;
        const char *name = glfwGetKeyName(k, 0);
        if (name && name[0] && !name[1]) {
            if (tolower((unsigned char)name[0]) == letter) return true;
        } else if (k == KEY_A + (letter - 'a')) {
            /* No layout name (e.g. non-Latin layout): fall back to the
             * physical US position, which is what macOS itself does. */
            return true;
        }
    }
    return false;
}
