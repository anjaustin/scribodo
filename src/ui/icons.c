/* SF Symbols as textures. See icons.h. AppKit is reached through the Objective-C runtime from plain C, as in
 * mac_window.c; every class and selector is checked before use. */
#include "ui/icons.h"

#include <objc/message.h>
#include <objc/runtime.h>
#include <stdlib.h>
#include <string.h>

typedef double IcFloat;
typedef struct { IcFloat w, h; } IcSize;
typedef struct { IcFloat x, y, w, h; } IcRect;

#define SEL_(s) sel_registerName(s)

static const char *const k_names[ICON_COUNT] = {
    "folder", "square.and.arrow.down", "magnifyingglass", "eye", "square.and.pencil", "doc.text",
    "bubble.left.and.bubble.right", "clock.arrow.circlepath", "note.text", "book.closed", "person.2",
};

static Texture2D g_tex[ICON_COUNT];
static Vector2 g_size[ICON_COUNT]; /* points */
static float g_loaded_pt, g_loaded_dpi;

static id cls(const char *c) { return (id)objc_getClass(c); }
static bool answers_class(id c, const char *sel) { return c && class_respondsToSelector(object_getClass(c), SEL_(sel)); }
static id ns_string(const char *s) { return ((id(*)(id, SEL, const char *))objc_msgSend)(cls("NSString"), SEL_("stringWithUTF8String:"), s); }

/* The symbol drawn into an RGBA bitmap at `scale` pixels per point; its alpha becomes a white texture's. */
static bool render(const char *name, float pt, float dpi, Texture2D *out, Vector2 *size)
{
    id image_class = cls("NSImage"), config_class = cls("NSImageSymbolConfiguration");
    if (!answers_class(image_class, "imageWithSystemSymbolName:accessibilityDescription:") ||
        !answers_class(config_class, "configurationWithPointSize:weight:"))
        return false;
    id img = ((id(*)(id, SEL, id, id))objc_msgSend)(image_class, SEL_("imageWithSystemSymbolName:accessibilityDescription:"),
                                                    ns_string(name), nil);
    if (!img) return false;
    id cfg = ((id(*)(id, SEL, IcFloat, IcFloat))objc_msgSend)(config_class, SEL_("configurationWithPointSize:weight:"),
                                                               (IcFloat)pt, 0.0 /* NSFontWeightRegular */);
    img = ((id(*)(id, SEL, id))objc_msgSend)(img, SEL_("imageWithSymbolConfiguration:"), cfg);
    if (!img) return false;
    IcSize sz = ((IcSize(*)(id, SEL))objc_msgSend)(img, SEL_("size"));
    long w = (long)(sz.w * dpi + 0.999), h = (long)(sz.h * dpi + 0.999);
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return false;
    id rep = ((id(*)(id, SEL))objc_msgSend)(cls("NSBitmapImageRep"), SEL_("alloc"));
    rep = ((id(*)(id, SEL, void *, long, long, long, long, BOOL, BOOL, id, long, long))objc_msgSend)(
        rep, SEL_("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:"),
        NULL, w, h, 8L, 4L, YES, NO, ns_string("NSDeviceRGBColorSpace"), 0L, 0L);
    if (!rep) return false;
    id ctx = ((id(*)(id, SEL, id))objc_msgSend)(cls("NSGraphicsContext"), SEL_("graphicsContextWithBitmapImageRep:"), rep);
    bool ok = false;
    if (ctx) {
        ((void (*)(id, SEL))objc_msgSend)(cls("NSGraphicsContext"), SEL_("saveGraphicsState"));
        ((void (*)(id, SEL, id))objc_msgSend)(cls("NSGraphicsContext"), SEL_("setCurrentContext:"), ctx);
        ((void (*)(id, SEL, IcRect))objc_msgSend)(img, SEL_("drawInRect:"), (IcRect){ 0, 0, (IcFloat)w, (IcFloat)h });
        ((void (*)(id, SEL))objc_msgSend)(cls("NSGraphicsContext"), SEL_("restoreGraphicsState"));
        const unsigned char *px = ((unsigned char *(*)(id, SEL))objc_msgSend)(rep, SEL_("bitmapData"));
        long bpr = ((long (*)(id, SEL))objc_msgSend)(rep, SEL_("bytesPerRow"));
        unsigned char *rgba = px && bpr >= w * 4 ? malloc((size_t)(w * h * 4)) : NULL;
        if (rgba) {
            for (long y = 0; y < h; y++)
                for (long x = 0; x < w; x++) {
                    unsigned char *o = rgba + (y * w + x) * 4;
                    o[0] = o[1] = o[2] = 255;
                    o[3] = px[y * bpr + x * 4 + 3];
                }
            Image im = { rgba, (int)w, (int)h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
            *out = LoadTextureFromImage(im);
            free(rgba);
            if (out->id) {
                SetTextureFilter(*out, TEXTURE_FILTER_BILINEAR);
                *size = (Vector2){ (float)sz.w, (float)sz.h };
                ok = true;
            }
        }
    }
    ((void (*)(id, SEL))objc_msgSend)(rep, SEL_("release"));
    return ok;
}

void icons_unload(void)
{
    for (int i = 0; i < ICON_COUNT; i++) {
        if (g_tex[i].id) UnloadTexture(g_tex[i]);
        g_tex[i] = (Texture2D){ 0 };
        g_size[i] = (Vector2){ 0, 0 };
    }
    g_loaded_pt = 0;
}

void icons_load(float pt, float dpi)
{
    if (pt == g_loaded_pt && dpi == g_loaded_dpi) return;
    icons_unload();
    id pool = cls("NSAutoreleasePool") ? ((id(*)(id, SEL))objc_msgSend)(((id(*)(id, SEL))objc_msgSend)(cls("NSAutoreleasePool"), SEL_("alloc")), SEL_("init")) : nil;
    for (int i = 0; i < ICON_COUNT; i++) render(k_names[i], pt, dpi > 0 ? dpi : 1, &g_tex[i], &g_size[i]);
    if (pool) ((void (*)(id, SEL))objc_msgSend)(pool, SEL_("drain"));
    g_loaded_pt = pt;
    g_loaded_dpi = dpi;
}

Vector2 icon_size(IconId id) { return id >= 0 && id < ICON_COUNT && g_tex[id].id ? g_size[id] : (Vector2){ 0, 0 }; }

const Texture2D *icon_texture(IconId id) { return id >= 0 && id < ICON_COUNT && g_tex[id].id ? &g_tex[id] : NULL; }

void icon_draw(IconId id, float x, float y, Color tint)
{
    const Texture2D *t = icon_texture(id);
    if (!t) return;
    DrawTexturePro(*t, (Rectangle){ 0, 0, (float)t->width, (float)t->height },
                   (Rectangle){ x, y, g_size[id].x, g_size[id].y }, (Vector2){ 0, 0 }, 0, tint);
}
