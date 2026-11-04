/* Scripted UI test runner.
 *
 * Drives the real app through raylib's input-event playback: key, modifier,
 * mouse, wheel, window-close, and resize events are injected into raylib's
 * input state at the start of a frame, so the app's normal input handling
 * runs unmodified. Typed text is injected through input_inject_chars(),
 * because raylib's playback cannot synthesise character events.
 *
 * Script format (one command per line, '#' comments, "quoted strings" with
 * \n \t \" \\ \u{XXXX} escapes; $DIR expands to the fixture directory):
 *
 *   open "path"                  open a file directly
 *   drop "path"                  open via the file-drop handler
 *   type "text"                  type characters
 *   key NAME [shift] [cmd] [alt] [ctrl]
 *                                press and release a key with modifiers
 *   click X Y [COUNT] [shift]    click at window coordinates
 *   click_index N [COUNT] [shift]
 *                                click at the caret position of index N
 *   drag X1 Y1 X2 Y2 [HOLD]      press, move, hold HOLD frames, release
 *   drag_index A B               drag from index A to index B
 *   move X Y | wheel DY | wait FRAMES | resize W H | close
 *   shot NAME                    save a screenshot after this frame draws
 *   clipboard "text"             set the (private) clipboard
 *   write "path" "content"       write a file (simulate another program)
 *   chmod "path" OCTAL
 *
 *   expect text "..."            expect selection ANCHOR CURSOR
 *   expect cursor N              expect dirty yes|no
 *   expect readonly yes|no       expect status "substring"
 *   expect file "path" "content" expect files "dir" "substring" COUNT
 *   expect clipboard "..."       expect visible N yes|no
 *   expect scrolled yes|no       expect explorer_w MIN MAX
 *   expect observer_w MIN MAX    expect editor_w MIN MAX
 *   expect running no            (only valid as the last command, after close)
 */
#ifndef SCRIBODO_SCRIPT_H
#define SCRIBODO_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>

#include "raylib.h"
#include "ui/editor.h"

typedef struct {
    Editor *editor;
    float *explorer_w;
    float *observer_w;
    Rectangle *editor_rect;
    const char *(*status)(void *ctx);
    void (*open)(void *ctx, const char *path);
    void (*drop)(void *ctx, const char *path);
    void *ctx;
} ScriptHost;

typedef struct Script Script;

Script *script_load(const char *path, const char *dir, const char *shot_dir, char *err, size_t errlen);
void script_free(Script *s);

/* Call at the start of every frame, before any input is read. Returns false
 * when the script has finished. */
bool script_frame(Script *s, ScriptHost *h);

/* Screenshot to save after this frame is drawn (and before EndDrawing), or
 * NULL. Clears the request. */
const char *script_take_shot(Script *s);

/* The app is about to exit. `app_closed` is true if it exited because the
 * window closed rather than because the script ended. Prints a summary and
 * returns the process exit code. */
int script_finish(Script *s, ScriptHost *h, bool app_closed);

#endif
