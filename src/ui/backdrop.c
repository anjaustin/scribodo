/* Frosted glass behind a dialog. See backdrop.h. */
#include "ui/backdrop.h"

#include <stdbool.h>
#include <stddef.h>

#include "rlgl.h"
#include "ui/theme.h"

/* One direction of a 9-tap Gaussian (sigma about 2 texels of the half-resolution copy), run twice. */
static const char k_blur_fs[] =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec2 step;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    const float w[5] = float[](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);\n"
    "    vec3 c = texture(texture0, fragTexCoord).rgb * w[0];\n"
    "    for (int i = 1; i < 5; i++) {\n"
    "        c += texture(texture0, fragTexCoord + step * float(i) * 1.6).rgb * w[i];\n"
    "        c += texture(texture0, fragTexCoord - step * float(i) * 1.6).rgb * w[i];\n"
    "    }\n"
    "    finalColor = vec4(c, 1.0);\n"
    "}\n";

static Shader g_blur;
static int g_step_loc = -1;
static bool g_shader_tried, g_shader_ok;
static RenderTexture2D g_a, g_b; /* half resolution: the blur's ping and pong */
static bool g_ready;
static int g_w, g_h; /* the window's render size the snapshot was taken at */

void backdrop_release(void)
{
    if (g_ready) {
        UnloadRenderTexture(g_a);
        UnloadRenderTexture(g_b);
    }
    g_ready = false;
}

static void blur_pass(RenderTexture2D src, RenderTexture2D dst, float dx, float dy)
{
    float step[2] = { dx / (float)src.texture.width, dy / (float)src.texture.height };
    BeginTextureMode(dst);
    BeginShaderMode(g_blur);
    SetShaderValue(g_blur, g_step_loc, step, SHADER_UNIFORM_VEC2);
    /* Render textures are stored bottom-up: a negative height draws them the right way round. */
    DrawTexturePro(src.texture, (Rectangle){ 0, 0, (float)src.texture.width, -(float)src.texture.height },
                   (Rectangle){ 0, 0, (float)dst.texture.width, (float)dst.texture.height }, (Vector2){ 0, 0 }, 0, THEME_NO_TINT);
    EndShaderMode();
    EndTextureMode();
}

static bool capture(void)
{
    int w = GetRenderWidth(), h = GetRenderHeight();
    if (w <= 0 || h <= 0) return false;
    rlDrawRenderBatchActive(); /* the panes drawn so far are in the frame buffer */
    Image shot = LoadImageFromScreen();
    if (!shot.data) return false;
    Texture2D full = LoadTextureFromImage(shot);
    UnloadImage(shot);
    if (!full.id) return false;
    SetTextureFilter(full, TEXTURE_FILTER_BILINEAR);
    g_a = LoadRenderTexture(w / 2, h / 2);
    g_b = LoadRenderTexture(w / 2, h / 2);
    SetTextureFilter(g_a.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_b.texture, TEXTURE_FILTER_BILINEAR);
    /* Downsample (the snapshot is top-down, unlike a render texture), then blur across and down, twice over. */
    BeginTextureMode(g_a);
    ClearBackground(THEME_BG);
    DrawTexturePro(full, (Rectangle){ 0, 0, (float)full.width, (float)full.height },
                   (Rectangle){ 0, 0, (float)g_a.texture.width, (float)g_a.texture.height }, (Vector2){ 0, 0 }, 0, THEME_NO_TINT);
    EndTextureMode();
    UnloadTexture(full);
    for (int round = 0; round < 2; round++) {
        blur_pass(g_a, g_b, 1, 0);
        blur_pass(g_b, g_a, 0, 1);
    }
    g_w = w, g_h = h;
    g_ready = true;
    return true;
}

void backdrop_draw(Rectangle screen)
{
    if (!g_shader_tried) {
        g_shader_tried = true;
        g_blur = LoadShaderFromMemory(NULL, k_blur_fs);
        g_shader_ok = IsShaderValid(g_blur);
        g_step_loc = g_shader_ok ? GetShaderLocation(g_blur, "step") : -1;
    }
    if (g_ready && (g_w != GetRenderWidth() || g_h != GetRenderHeight())) backdrop_release(); /* resized */
    if (g_shader_ok && !g_ready) capture();
    if (g_ready) {
        DrawTexturePro(g_a.texture, (Rectangle){ 0, 0, (float)g_a.texture.width, -(float)g_a.texture.height }, screen,
                       (Vector2){ 0, 0 }, 0, THEME_NO_TINT);
        Color veil = THEME_SCRIM;
        veil.a = (unsigned char)(veil.a * 0.55f); /* the blur does most of the separating; a lighter veil keeps colour */
        DrawRectangleRec(screen, veil);
    } else {
        DrawRectangleRec(screen, THEME_SCRIM);
    }
}
