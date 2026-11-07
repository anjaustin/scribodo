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
 *   click_element NAME [COUNT]   click the centre of a named UI element
 *   wait_chat [MAX_FRAMES]       wait until no chat reply is streaming
 *   wait_history [MAX_FRAMES]    wait until history completion is applied/rejected
 *   history_hold yes|no         script-only delayed history reader
 *   measure_frames start|stop   reset/report bounded interframe histogram;
 *                              includes waits/vsync, not isolated CPU/input time
 *   store_restart               script-only manager restart (no service shutdown)
 *   expect history_busy yes|no  a history replacement is pending
 *   wait_ready [MAX_FRAMES]      wait until the model is ready (fails on timeout)
 *   move X Y | wheel DY | wait FRAMES | resize W H | close
 *   shot NAME                    save a screenshot after this frame draws
 *   clipboard "text"             set the (private) clipboard
 *   write "path" "content"       write a file (simulate another program)
 *   chmod "path" OCTAL           mkdir "path"
 *
 *   expect text "..."            expect selection ANCHOR CURSOR
 *   expect cursor N              expect dirty yes|no
 *   expect readonly yes|no       expect status "substring"
 *   expect file "path" "content" expect files "dir" "substring" COUNT
 *   expect clipboard "..."       expect visible N yes|no
 *   expect scrolled yes|no       expect explorer_w MIN MAX
 *   expect observer_w MIN MAX    expect editor_w MIN MAX
 *   expect chat_count N          expect chat_last "substring"
 *   expect chat_busy yes|no       expect chat_last_not "substring"
 *   expect element_w NAME MIN MAX (width of a named element; 0 if absent)
 *   expect lines "path" "substring" COUNT  (lines of a file containing it)
 *   expect record "substring"    (the open note/canon panel: header + text)
 *   observer on|off              (scripts start with the observer off)
 *   dialog "answer"              the next native dialog's answer ("" = Cancel;
 *                                several files separated by '|')
 *   right_click_element NAME     right-click a named element (context menus)
 *   wait_observer [MAX_FRAMES]   wait until the observer has nothing to do
 *   expect findings_count N      expect findings_has "type|status|text substring"
 *   expect findings_not "substring"
 *   expect attached "a.md,b.md"  the chat's attached documents, in order
 *   expect explorer_has "row"    a row the Explorer lists (exact)
 *   expect observer_status "substring"
 *   expect running no            (only valid as the last command, after close)
 *   store_offline yes|no         force this test's StoreManager offline/online
 *   expect chat_unsaved yes|no   whether DB turns lack committed sequence receipts
 *   wait_memory SEQ|off [FRAMES] wait for durable ingestion/outbox or disable
 *   expect memory_enabled yes|no
 *   expect memory_sources "..." expect memory_sources_not "..." (as-of snapshot)
 */
#ifndef SCRIBODO_SCRIPT_H
#define SCRIBODO_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>

#include "llm/chat.h"
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
    ChatSession *chat;
    /* Screen rect of a named UI element ("ChatInput", "ChatItem0", ...). */
    Rectangle (*element)(void *ctx, const char *name);
    /* True once the model can take requests (always true for the fake). */
    bool (*model_ready)(void *ctx);
    /* The open note/canon panel as "header\ntext" (malloc'd), or "". */
    char *(*record_text)(void *ctx);
    /* Observer: enable/disable (scripts start with it off), idle check, and
     * the listed findings as "type|status|description\n" lines (malloc'd). */
    void (*observer_enable)(void *ctx, bool on);
    bool (*observer_idle)(void *ctx);
    char *(*findings_text)(void *ctx);
    /* The chat's attached documents ("a.md,b.md") and the Explorer's rows
     * (one per line), malloc'd. */
    char *(*attached_text)(void *ctx);
    char *(*explorer_text)(void *ctx);
    /* The observer's status line (malloc'd). */
    char *(*observer_status)(void *ctx);
    size_t (*history_cache_bytes)(void *ctx);
    void (*store_offline)(void *ctx, bool offline); /* scripted service failure, not a production menu */
    bool (*history_busy)(void *ctx);
    void (*history_hold)(void *ctx, bool hold);
    bool (*history_worker_busy)(void *ctx);
    void (*store_restart)(void *ctx);
    bool (*memory_settled)(void *ctx, long sequence);
    bool (*memory_enabled)(void *ctx);
    char *(*memory_sources)(void *ctx); /* owned as-of source snapshot */
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
