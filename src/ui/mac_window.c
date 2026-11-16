/* The window as a Mac app's. See mac_window.h. Every call goes through objc_msgSend with the exact signature of the
 * method it sends, and every object is checked to answer the selector first, so an unexpected window degrades to the
 * plain title bar instead of crashing. */
#include "ui/mac_window.h"

#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string.h>

typedef struct { double x, y; } MwPoint;
typedef struct { double w, h; } MwSize;
typedef struct { MwPoint origin; MwSize size; } MwRect;

#define SEL_(s) sel_registerName(s)
#define NS_FULL_SIZE_CONTENT_VIEW (1ul << 15)
#define NS_TITLE_HIDDEN 1L
#define NS_LEFT_MOUSE_DOWN 1UL
#define NS_LEFT_MOUSE_UP 2UL
#define BUTTON_SPACING 23.0 /* AppKit's own, between the buttons' left edges */
#define BUTTON_LEFT 14.0

static bool g_unified;
static float g_inset;

static bool answers(id o, const char *sel) { return o && class_respondsToSelector(object_getClass(o), SEL_(sel)); }
static id send_id(id o, const char *sel) { return ((id(*)(id, SEL))objc_msgSend)(o, SEL_(sel)); }
static MwRect frame_of(id v) { return ((MwRect(*)(id, SEL))objc_msgSend)(v, SEL_("frame")); }

static id window_button(id win, unsigned long which)
{
    return ((id(*)(id, SEL, unsigned long))objc_msgSend)(win, SEL_("standardWindowButton:"), which);
}

bool mac_window_unify(void *nswindow, float toolbar_h)
{
    id win = (id)nswindow;
    if (!answers(win, "setTitlebarAppearsTransparent:") || !answers(win, "styleMask") ||
        !answers(win, "standardWindowButton:"))
        return false;
    unsigned long mask = ((unsigned long (*)(id, SEL))objc_msgSend)(win, SEL_("styleMask"));
    ((void (*)(id, SEL, unsigned long))objc_msgSend)(win, SEL_("setStyleMask:"), mask | NS_FULL_SIZE_CONTENT_VIEW);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(win, SEL_("setTitlebarAppearsTransparent:"), YES);
    ((void (*)(id, SEL, long))objc_msgSend)(win, SEL_("setTitleVisibility:"), NS_TITLE_HIDDEN);
    g_unified = true;
    g_inset = (float)(BUTTON_LEFT + 2 * BUTTON_SPACING + 14.0 + 18.0); /* the last button's right edge, then a gap */
    mac_window_place_buttons(nswindow, toolbar_h);
    return true;
}

void mac_window_place_buttons(void *nswindow, float toolbar_h)
{
    id win = (id)nswindow;
    if (!g_unified || !win) return;
    for (unsigned long i = 0; i < 3; i++) {
        id b = window_button(win, i);
        if (!answers(b, "setFrameOrigin:")) continue;
        id sup = send_id(b, "superview");
        if (!answers(sup, "isFlipped")) continue;
        MwRect sf = frame_of(sup), f = frame_of(b);
        BOOL flipped = ((BOOL(*)(id, SEL))objc_msgSend)(sup, SEL_("isFlipped"));
        double cy = toolbar_h / 2.0;
        MwPoint want = { BUTTON_LEFT + (double)i * BUTTON_SPACING, flipped ? cy - f.size.h / 2 : sf.size.h - cy - f.size.h / 2 };
        if (want.y < 0) want.y = 0; /* a title bar shorter than half the toolbar: as low as it goes */
        if (f.origin.x != want.x || f.origin.y != want.y)
            ((void (*)(id, SEL, MwPoint))objc_msgSend)(b, SEL_("setFrameOrigin:"), want);
    }
}

float mac_window_toolbar_inset(void) { return g_unified ? g_inset : 0.0f; }

bool mac_window_begin_drag(void *nswindow)
{
    id win = (id)nswindow;
    if (!g_unified || !answers(win, "performWindowDragWithEvent:")) return false;
    id app = send_id((id)objc_getClass("NSApplication"), "sharedApplication");
    id ev = answers(app, "currentEvent") ? send_id(app, "currentEvent") : nil;
    if (!answers(ev, "type")) return false;
    unsigned long type = ((unsigned long (*)(id, SEL))objc_msgSend)(ev, SEL_("type"));
    if (type != NS_LEFT_MOUSE_DOWN) return false; /* only the press itself starts the system's drag */
    MwPoint at = ((MwPoint(*)(id, SEL))objc_msgSend)(ev, SEL_("locationInWindow"));
    ((void (*)(id, SEL, id))objc_msgSend)(win, SEL_("performWindowDragWithEvent:"), ev);
    /* The system's drag loop consumes the release, so GLFW (and raylib) would still hold the button down and lose the
     * next click: a release is posted for them. */
    id info = send_id((id)objc_getClass("NSProcessInfo"), "processInfo");
    double now = answers(info, "systemUptime") ? ((double (*)(id, SEL))objc_msgSend)(info, SEL_("systemUptime")) : 0;
    long number = ((long (*)(id, SEL))objc_msgSend)(win, SEL_("windowNumber"));
    id up = ((id(*)(id, SEL, unsigned long, MwPoint, unsigned long, double, long, id, long, long, float))objc_msgSend)(
        (id)objc_getClass("NSEvent"), SEL_("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:"),
        NS_LEFT_MOUSE_UP, at, 0UL, now, number, nil, 0L, 1L, 0.0f);
    if (up) ((void (*)(id, SEL, id, BOOL))objc_msgSend)(app, SEL_("postEvent:atStart:"), up, NO);
    return true;
}

void mac_window_title_double_click(void *nswindow)
{
    id win = (id)nswindow;
    if (!g_unified || !win) return;
    /* NSGlobalDomain AppleActionOnDoubleClick: "Maximize" (the default), "Minimize" or "None". */
    char action[32] = "Maximize";
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("AppleActionOnDoubleClick"), kCFPreferencesAnyApplication);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, action, sizeof(action), kCFStringEncodingUTF8);
    if (v) CFRelease(v);
    if (!strcmp(action, "None")) return;
    const char *sel = !strcmp(action, "Minimize") ? "performMiniaturize:" : "performZoom:";
    if (answers(win, sel)) ((void (*)(id, SEL, id))objc_msgSend)(win, SEL_(sel), nil);
}
