/* The window as a Mac app's. See mac_window.h. Every call goes through objc_msgSend with the exact signature of the
 * method it sends, and every object is checked to answer the selector first, so an unexpected window degrades to the
 * plain title bar instead of crashing.
 *
 * Struct returns (NSRect, NSPoint, NSSize) go through plain objc_msgSend, which is correct on arm64 only; x86_64 would
 * need objc_msgSend_stret for the rectangle. The MVP target is Apple Silicon, and the build refuses anything else. */
#include "ui/mac_window.h"

#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string.h>

#if !defined(__arm64__) && !defined(__aarch64__)
#error "ui/mac_window.c sends struct-returning messages through objc_msgSend, which is right on arm64 only"
#endif

typedef struct { double x, y; } MwPoint;
typedef struct { double w, h; } MwSize;
typedef struct { MwPoint origin; MwSize size; } MwRect;

#define SEL_(s) sel_registerName(s)
#define NS_FULL_SIZE_CONTENT_VIEW (1ul << 15)
#define NS_FULL_SCREEN (1ul << 14)
#define NS_TITLE_HIDDEN 1L
#define NS_TOOLBAR_STYLE_UNIFIED_COMPACT 4L
#define NS_LEFT_MOUSE_DOWN 1UL
#define NS_LEFT_MOUSE_UP 2UL
#define NS_LEFT_MOUSE_DRAGGED 6UL
#define INSET_GAP 18.0 /* between the zoom button and the toolbar's first item */

static bool g_unified;
static float g_toolbar_h;
/* The last left press in this app, from a local event monitor: the event the system's drag must start from. The event
 * in hand at frame time is whatever came last in the poll (a drag, or on a Force Touch trackpad a pressure event). */
static id g_last_down;

static bool answers(id o, const char *sel) { return o && class_respondsToSelector(object_getClass(o), SEL_(sel)); }
static id send_id(id o, const char *sel) { return ((id(*)(id, SEL))objc_msgSend)(o, SEL_(sel)); }
static MwRect frame_of(id v) { return ((MwRect(*)(id, SEL))objc_msgSend)(v, SEL_("frame")); }
static id cls(const char *name) { return (id)objc_getClass(name); }

static id window_button(id win, unsigned long which)
{
    return ((id(*)(id, SEL, unsigned long))objc_msgSend)(win, SEL_("standardWindowButton:"), which);
}

static bool full_screen(id win)
{
    return win && answers(win, "styleMask") && (((unsigned long (*)(id, SEL))objc_msgSend)(win, SEL_("styleMask")) & NS_FULL_SCREEN);
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
    /* An empty toolbar in the unified compact style makes AppKit's own title bar 40 pt tall with the buttons centred in
     * it, and keeps them there through resizing, zooming and full screen (measured 2026-11-16: the close button's centre
     * 20 pt from the top before and after a resize; moving them by hand was undone by every resize). Clicks in the band
     * still reach the app's toolbar beneath. */
    id toolbar_class = cls("NSToolbar");
    if (answers(win, "setToolbarStyle:") && toolbar_class) {
        id tb = ((id(*)(id, SEL, id))objc_msgSend)(send_id(toolbar_class, "alloc"), SEL_("initWithIdentifier:"),
                                                   ((id(*)(id, SEL, const char *))objc_msgSend)(cls("NSString"), SEL_("stringWithUTF8String:"), "scribodo"));
        if (tb) {
            if (answers(tb, "setShowsBaselineSeparator:")) ((void (*)(id, SEL, BOOL))objc_msgSend)(tb, SEL_("setShowsBaselineSeparator:"), NO);
            ((void (*)(id, SEL, id))objc_msgSend)(win, SEL_("setToolbar:"), tb);
            ((void (*)(id, SEL, long))objc_msgSend)(win, SEL_("setToolbarStyle:"), NS_TOOLBAR_STYLE_UNIFIED_COMPACT);
            ((void (*)(id, SEL))objc_msgSend)(tb, SEL_("release")); /* the window holds it */
        }
    }
    id event_class = cls("NSEvent");
    if (answers(event_class, "addLocalMonitorForEventsMatchingMask:handler:")) {
        id (^keep)(id) = ^id(id ev) {
            if (g_last_down) ((void (*)(id, SEL))objc_msgSend)(g_last_down, SEL_("release"));
            g_last_down = ev ? ((id(*)(id, SEL))objc_msgSend)(ev, SEL_("retain")) : nil;
            return ev;
        };
        const unsigned long long left_mouse_down_mask = 1ULL << NS_LEFT_MOUSE_DOWN;
        (void)((id(*)(id, SEL, unsigned long long, id))objc_msgSend)(event_class, SEL_("addLocalMonitorForEventsMatchingMask:handler:"),
                                                                     left_mouse_down_mask, (id)keep);
    }
    g_unified = true;
    g_toolbar_h = toolbar_h;
    return true;
}

void mac_window_place_buttons(void *nswindow, float toolbar_h)
{
    /* In full screen AppKit lifts the toolbar into a window of its own over the app's toolbar band, which would take
     * its clicks (red team): the toolbar is hidden there and shown again on the way out. */
    id win = (id)nswindow;
    if (!g_unified || !win) return;
    id tb = answers(win, "toolbar") ? send_id(win, "toolbar") : nil;
    if (answers(tb, "isVisible")) {
        bool want = !full_screen(win);
        if ((bool)((BOOL(*)(id, SEL))objc_msgSend)(tb, SEL_("isVisible")) != want)
            ((void (*)(id, SEL, BOOL))objc_msgSend)(tb, SEL_("setVisible:"), want ? YES : NO);
    }
    if (full_screen(win)) return;
    /* With the compact toolbar AppKit centres the buttons itself; only a title bar of another height (an older macOS
     * without toolbar styles) needs them moved, and then only vertically. */
    for (unsigned long i = 0; i < 3; i++) {
        id b = window_button(win, i);
        if (!answers(b, "setFrameOrigin:")) continue;
        id sup = send_id(b, "superview");
        if (!answers(sup, "isFlipped")) continue;
        MwRect sf = frame_of(sup), f = frame_of(b);
        BOOL flipped = ((BOOL(*)(id, SEL))objc_msgSend)(sup, SEL_("isFlipped"));
        double cy = toolbar_h / 2.0;
        double want_y = flipped ? cy - f.size.h / 2 : sf.size.h - cy - f.size.h / 2;
        if (want_y < 0) want_y = 0;
        if (f.origin.y - want_y > 1.0 || want_y - f.origin.y > 1.0)
            ((void (*)(id, SEL, MwPoint))objc_msgSend)(b, SEL_("setFrameOrigin:"), (MwPoint){ f.origin.x, want_y });
    }
}

float mac_window_toolbar_inset(void *nswindow)
{
    id win = (id)nswindow;
    if (!g_unified || !win || full_screen(win)) return 0.0f; /* full screen: the buttons are not in the window */
    id zoom = window_button(win, 2);
    id sup = zoom ? send_id(zoom, "superview") : nil;
    if (!answers(sup, "convertPoint:toView:")) return 90.0f;
    MwRect f = frame_of(zoom);
    MwPoint right = ((MwPoint(*)(id, SEL, MwPoint, id))objc_msgSend)(sup, SEL_("convertPoint:toView:"),
                                                                   (MwPoint){ f.origin.x + f.size.w, f.origin.y }, nil);
    return (float)(right.x + INSET_GAP);
}

bool mac_window_begin_drag(void *nswindow)
{
    id win = (id)nswindow;
    if (!g_unified || !answers(win, "performWindowDragWithEvent:")) return false;
    id app = send_id(cls("NSApplication"), "sharedApplication");
    /* The press itself, from the monitor (else the event in hand, when it is that press or a drag after it). It must be
     * this window's, recent, and the button still down, or nothing happens. */
    id ev = g_last_down;
    if (!answers(ev, "timestamp")) ev = answers(app, "currentEvent") ? send_id(app, "currentEvent") : nil;
    if (!answers(ev, "type") || !answers(ev, "window") || !answers(ev, "timestamp")) return false;
    unsigned long type = ((unsigned long (*)(id, SEL))objc_msgSend)(ev, SEL_("type"));
    unsigned long buttons = ((unsigned long (*)(id, SEL))objc_msgSend)(cls("NSEvent"), SEL_("pressedMouseButtons"));
    id info0 = send_id(cls("NSProcessInfo"), "processInfo");
    double age = (answers(info0, "systemUptime") ? ((double (*)(id, SEL))objc_msgSend)(info0, SEL_("systemUptime")) : 0) -
                 ((double (*)(id, SEL))objc_msgSend)(ev, SEL_("timestamp"));
    if ((type != NS_LEFT_MOUSE_DOWN && type != NS_LEFT_MOUSE_DRAGGED) || send_id(ev, "window") != win || !(buttons & 1UL) ||
        age > 1.0)
        return false;
    MwPoint at = ((MwPoint(*)(id, SEL))objc_msgSend)(ev, SEL_("locationInWindow"));
    ((void (*)(id, SEL, id))objc_msgSend)(win, SEL_("performWindowDragWithEvent:"), ev);
    /* The system's drag loop consumes the release, so GLFW (and raylib) would still hold the button down and lose the
     * next click: a release is posted for them. */
    id info = send_id(cls("NSProcessInfo"), "processInfo");
    double now = answers(info, "systemUptime") ? ((double (*)(id, SEL))objc_msgSend)(info, SEL_("systemUptime")) : 0;
    long number = ((long (*)(id, SEL))objc_msgSend)(win, SEL_("windowNumber"));
    id up = ((id(*)(id, SEL, unsigned long, MwPoint, unsigned long, double, long, id, long, long, float))objc_msgSend)(
        cls("NSEvent"), SEL_("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:"),
        NS_LEFT_MOUSE_UP, at, 0UL, now, number, nil, 0L, 1L, 0.0f);
    if (up) ((void (*)(id, SEL, id, BOOL))objc_msgSend)(app, SEL_("postEvent:atStart:"), up, NO);
    return true;
}

int mac_window_last_click_count(void *nswindow)
{
    id ev = g_last_down;
    if (!answers(ev, "clickCount") || send_id(ev, "window") != (id)nswindow) return 0;
    return (int)((long (*)(id, SEL))objc_msgSend)(ev, SEL_("clickCount"));
}

void mac_window_title_double_click(void *nswindow)
{
    id win = (id)nswindow;
    if (!g_unified || !win) return;
    /* NSGlobalDomain AppleActionOnDoubleClick: "Maximize" (the default), "Fill", "Minimize" or "None". */
    char action[32] = "Maximize";
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("AppleActionOnDoubleClick"), kCFPreferencesAnyApplication);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, action, sizeof(action), kCFStringEncodingUTF8);
    if (v) CFRelease(v);
    if (!strcmp(action, "None")) return;
    const char *sel = !strcmp(action, "Minimize") ? "performMiniaturize:" : "performZoom:"; /* "Fill" too: AppKit has no call for it */
    if (answers(win, sel)) ((void (*)(id, SEL, id))objc_msgSend)(win, SEL_(sel), nil);
}
