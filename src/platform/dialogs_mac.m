#import <Cocoa/Cocoa.h>

#include <stdlib.h>
#include <string.h>

#include "dialogs.h"

static bool g_scripted;
static char g_answer[4096];
static bool g_have_answer;

void dialogs_script_mode(bool on) { g_scripted = on; }

void dialogs_script_answer(const char *answer)
{
    snprintf(g_answer, sizeof(g_answer), "%s", answer ? answer : "");
    g_have_answer = true;
}

static bool take_answer(char *out, size_t outlen)
{
    bool have = g_have_answer && g_answer[0];
    if (have) snprintf(out, outlen, "%s", g_answer);
    g_have_answer = false;
    return have;
}

/* The panel takes key focus; give it back to our window afterwards. */
static void restore_focus(NSWindow *prev)
{
    if (prev) [prev makeKeyAndOrderFront:nil];
}

bool dialogs_choose_folder(const char *message, const char *prompt, bool allow_create, char *out, size_t outlen)
{
    if (g_scripted) return take_answer(out, outlen);
    @autoreleasepool {
        NSWindow *prev = [NSApp keyWindow];
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseFiles = NO;
        p.canChooseDirectories = YES;
        p.canCreateDirectories = allow_create;
        p.allowsMultipleSelection = NO;
        if (message) p.message = [NSString stringWithUTF8String:message];
        if (prompt) p.prompt = [NSString stringWithUTF8String:prompt];
        bool ok = [p runModal] == NSModalResponseOK && p.URL;
        if (ok) snprintf(out, outlen, "%s", p.URL.fileSystemRepresentation);
        restore_focus(prev);
        return ok;
    }
}

size_t dialogs_choose_files(const char *message, char ***paths)
{
    *paths = NULL;
    if (g_scripted) {
        char buf[4096];
        if (!take_answer(buf, sizeof(buf))) return 0;
        size_t n = 1;
        for (char *c = buf; *c; c++) n += *c == '|';
        char **list = calloc(n, sizeof(char *));
        size_t k = 0;
        for (char *tok = strtok(buf, "|"); tok && list && k < n; tok = strtok(NULL, "|")) list[k++] = strdup(tok);
        *paths = list;
        return list ? k : 0;
    }
    @autoreleasepool {
        NSWindow *prev = [NSApp keyWindow];
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseFiles = YES;
        p.canChooseDirectories = NO;
        p.allowsMultipleSelection = YES;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        p.allowedFileTypes = @[ @"md", @"markdown", @"txt" ];
#pragma clang diagnostic pop
        if (message) p.message = [NSString stringWithUTF8String:message];
        p.prompt = @"Import";
        size_t n = 0;
        if ([p runModal] == NSModalResponseOK) {
            NSArray<NSURL *> *urls = p.URLs;
            char **list = calloc(urls.count ? urls.count : 1, sizeof(char *));
            for (NSURL *u in urls)
                if (list) list[n++] = strdup(u.fileSystemRepresentation);
            *paths = list;
        }
        restore_focus(prev);
        return n;
    }
}

void dialogs_free_paths(char **paths, size_t n)
{
    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

void dialogs_reveal(const char *path)
{
    if (g_scripted) return;
    @autoreleasepool {
        NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ u ]];
    }
}
