#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "script.h"
#include "frame_metrics.h"

#include "platform/dialogs.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>

#include "core/utf8.h"
#include "ui/input.h"
#include "core/deadline_clock.h"

/* raylib's AutomationEventType is private to rcore.c; these values mirror
 * the pinned raylib 6.2 enum (third_party/raylib/src/rcore.c). */
enum {
    EV_KEY_UP = 1,
    EV_KEY_DOWN = 2,
    EV_MOUSE_BUTTON_UP = 5,
    EV_MOUSE_BUTTON_DOWN = 6,
    EV_MOUSE_POSITION = 7,
    EV_MOUSE_WHEEL = 8,
    EV_WINDOW_CLOSE = 18,
    EV_WINDOW_RESIZE = 21,
};

#define MAX_TOKENS 8
#define MAX_BATCH_EVENTS 12
#define MAX_BATCHES 512
#define TEXT_MAX 4096

typedef struct {
    AutomationEvent ev[MAX_BATCH_EVENTS];
    int nev;
    uint32_t *chars; /* injected text (malloc'd), or NULL */
    size_t nchars;
} Batch;

struct Script {
    char **lines;
    int nlines;
    int pc;
    char dir[1024];
    char shot_dir[1024];
    Batch batches[MAX_BATCHES];
    int qhead, qcount;
    int wait;
    int wait_chat;      /* frames left to wait for the chat to go idle */
    int wait_history;
    int wait_recall;
    int wait_memory;
    long memory_sequence;
    int wait_memory_documents;
    long memory_documents_count;
    int wait_memory_briefs;
    long memory_briefs_count;
    int wait_status;    /* frames left to wait for the status to contain wait_text */
    char wait_text[256];
    int wait_observer;  /* frames left to wait for the observer to go idle */
    int wait_ready;     /* frames left to wait for the model */
    char shot[1100];
    bool shot_pending;
    int passed, failed;
    bool expect_exit; /* saw "expect running no" */
    int expect_exit_line;
    /* Heartbeat watchdog: the main loop bumps `frames`; if it stops moving
     * for STALL_LIMIT_S, the watchdog reports the script line and aborts. */
    atomic_ulong frames;
    atomic_int line;
    atomic_bool stop_watchdog;
    pthread_t watchdog;
    bool watchdog_running;
    FrameMetrics frame_metrics;
    bool measuring_frames;
    double frame_started_s;
};

#define STALL_LIMIT_S 15

static double frame_now(void)
{
    struct timespec ts;
    if (clock_gettime(SCRIBODO_DEADLINE_CLOCK, &ts) != 0) return NAN;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *watchdog_main(void *arg)
{
    Script *s = arg;
    unsigned long last = atomic_load(&s->frames);
    int stalled = 0;
    while (!atomic_load(&s->stop_watchdog)) {
        sleep(1);
        unsigned long now = atomic_load(&s->frames);
        stalled = now == last ? stalled + 1 : 0;
        last = now;
        if (stalled >= STALL_LIMIT_S) {
            int ln = atomic_load(&s->line);
            fprintf(stderr, "SCRIPT WATCHDOG: no frame for %d s; main loop stuck while executing line %d: %s\n",
                    STALL_LIMIT_S, ln + 1, ln >= 0 && ln < s->nlines ? s->lines[ln] : "?");
            printf("  FAIL %3d  main loop hung (no frame for %d s)\n", ln + 1, STALL_LIMIT_S);
            fflush(stdout);
            fflush(stderr);
            abort(); /* leaves a crash report with every thread's stack */
        }
    }
    return NULL;
}

/* ---- parsing --------------------------------------------------------------- */

typedef struct {
    char *tok[MAX_TOKENS];
    bool quoted[MAX_TOKENS];
    int n;
} Tokens;

static void append(char *out, size_t *o, size_t cap, const char *s, size_t n)
{
    if (*o + n >= cap) n = cap - *o - 1;
    memcpy(out + *o, s, n);
    *o += n;
}

/* Split `line` into tokens; quoted tokens get escapes and $DIR expanded. */
static bool tokenize(const Script *s, const char *line, Tokens *t, char *storage, size_t cap)
{
    t->n = 0;
    size_t o = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        if (t->n == MAX_TOKENS) return false;
        t->tok[t->n] = storage + o;
        t->quoted[t->n] = *p == '"';
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    p++;
                    char c = *p++;
                    if (c == 'n') append(storage, &o, cap, "\n", 1);
                    else if (c == 't') append(storage, &o, cap, "\t", 1);
                    else if (c == 'r') append(storage, &o, cap, "\r", 1);
                    else if (c == 'u' && *p == '{') {
                        uint32_t cp = (uint32_t)strtoul(p + 1, (char **)&p, 16);
                        if (*p == '}') p++;
                        uint8_t buf[4];
                        int k = utf8_encode_one(cp, buf);
                        append(storage, &o, cap, (const char *)buf, (size_t)k);
                    } else append(storage, &o, cap, &c, 1);
                } else if (*p == '$' && strncmp(p, "$DIR", 4) == 0) {
                    append(storage, &o, cap, s->dir, strlen(s->dir));
                    p += 4;
                } else {
                    append(storage, &o, cap, p++, 1);
                }
            }
            if (*p != '"') return false;
            p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') append(storage, &o, cap, p++, 1);
        }
        storage[o++] = '\0';
        t->n++;
    }
    return true;
}

Script *script_load(const char *path, const char *dir, const char *shot_dir, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open script %s", path);
        return NULL;
    }
    Script *s = calloc(1, sizeof(Script));
    snprintf(s->dir, sizeof(s->dir), "%s", dir ? dir : ".");
    snprintf(s->shot_dir, sizeof(s->shot_dir), "%s", shot_dir ? shot_dir : s->dir);
    char line[TEXT_MAX];
    int cap = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (s->nlines == cap) {
            cap = cap ? cap * 2 : 64;
            s->lines = realloc(s->lines, (size_t)cap * sizeof(char *));
        }
        s->lines[s->nlines++] = strdup(line);
    }
    fclose(f);
    atomic_init(&s->frames, 0);
    atomic_init(&s->line, -1);
    atomic_init(&s->stop_watchdog, false);
    s->watchdog_running = pthread_create(&s->watchdog, NULL, watchdog_main, s) == 0;
    return s;
}

void script_free(Script *s)
{
    if (!s) return;
    if (s->watchdog_running) {
        atomic_store(&s->stop_watchdog, true);
        pthread_join(s->watchdog, NULL);
    }
    for (int i = 0; i < s->nlines; i++) free(s->lines[i]);
    free(s->lines);
    for (int i = 0; i < s->qcount; i++) free(s->batches[(s->qhead + i) % MAX_BATCHES].chars);
    free(s);
}

/* ---- event batches --------------------------------------------------------- */

static Batch *new_batch(Script *s)
{
    if (s->qcount == MAX_BATCHES) return NULL;
    Batch *b = &s->batches[(s->qhead + s->qcount) % MAX_BATCHES];
    memset(b, 0, sizeof(*b));
    s->qcount++;
    return b;
}

static void add_ev(Batch *b, unsigned type, int p0, int p1)
{
    if (!b || b->nev == MAX_BATCH_EVENTS) return;
    b->ev[b->nev++] = (AutomationEvent){ .frame = 0, .type = type, .params = { p0, p1, 0, 0 } };
}

static void play_next_batch(Script *s)
{
    Batch *b = &s->batches[s->qhead];
    for (int i = 0; i < b->nev; i++) PlayAutomationEvent(b->ev[i]);
    if (b->chars) {
        input_inject_chars(b->chars, b->nchars);
        free(b->chars);
    }
    s->qhead = (s->qhead + 1) % MAX_BATCHES;
    s->qcount--;
}

static int key_code(const char *name)
{
    static const struct { const char *n; int k; } keys[] = {
        { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT }, { "UP", KEY_UP }, { "DOWN", KEY_DOWN },
        { "HOME", KEY_HOME }, { "END", KEY_END }, { "PGUP", KEY_PAGE_UP }, { "PGDN", KEY_PAGE_DOWN },
        { "BACKSPACE", KEY_BACKSPACE }, { "DELETE", KEY_DELETE }, { "ENTER", KEY_ENTER },
        { "TAB", KEY_TAB }, { "ESC", KEY_ESCAPE }, { "SPACE", KEY_SPACE },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strcmp(name, keys[i].n) == 0) return keys[i].k;
    if (name[0] && !name[1]) {
        if (name[0] >= 'A' && name[0] <= 'Z') return KEY_A + (name[0] - 'A');
        if (name[0] >= 'a' && name[0] <= 'z') return KEY_A + (name[0] - 'a');
        if (name[0] >= '0' && name[0] <= '9') return KEY_ZERO + (name[0] - '0');
    }
    return -1;
}

static int mod_code(const char *name)
{
    if (!strcmp(name, "shift")) return KEY_LEFT_SHIFT;
    if (!strcmp(name, "cmd")) return KEY_LEFT_SUPER;
    if (!strcmp(name, "alt") || !strcmp(name, "opt")) return KEY_LEFT_ALT;
    if (!strcmp(name, "ctrl")) return KEY_LEFT_CONTROL;
    return -1;
}

static void queue_click_button(Script *s, int x, int y, int count, bool shift, int button)
{
    for (int c = 0; c < count; c++) {
        Batch *down = new_batch(s);
        add_ev(down, EV_MOUSE_POSITION, x, y);
        if (shift) add_ev(down, EV_KEY_DOWN, KEY_LEFT_SHIFT, 0);
        add_ev(down, EV_MOUSE_BUTTON_DOWN, button, 0);
        Batch *up = new_batch(s);
        add_ev(up, EV_MOUSE_BUTTON_UP, button, 0);
        if (shift) add_ev(up, EV_KEY_UP, KEY_LEFT_SHIFT, 0);
    }
    new_batch(s); /* settle frame */
}

static void queue_click(Script *s, int x, int y, int count, bool shift)
{
    queue_click_button(s, x, y, count, shift, MOUSE_BUTTON_LEFT);
}

static void queue_drag(Script *s, int x1, int y1, int x2, int y2, int hold)
{
    Batch *b = new_batch(s);
    add_ev(b, EV_MOUSE_POSITION, x1, y1);
    add_ev(b, EV_MOUSE_BUTTON_DOWN, MOUSE_BUTTON_LEFT, 0);
    const int steps = 8;
    for (int i = 1; i <= steps; i++) {
        b = new_batch(s);
        add_ev(b, EV_MOUSE_POSITION, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
    }
    for (int i = 0; i < hold; i++) new_batch(s);
    b = new_batch(s);
    add_ev(b, EV_MOUSE_BUTTON_UP, MOUSE_BUTTON_LEFT, 0);
    new_batch(s);
}

/* ---- reporting --------------------------------------------------------------- */

static void report(Script *s, bool ok, const char *detail)
{
    const char *line = s->lines[s->pc];
    while (*line == ' ' || *line == '\t') line++;
    if (ok) {
        s->passed++;
        printf("  ok   %3d  %s\n", s->pc + 1, line);
    } else {
        s->failed++;
        printf("  FAIL %3d  %s\n", s->pc + 1, line);
        if (detail && *detail) printf("             %s\n", detail);
    }
}

static bool yes(const char *t) { return !strcmp(t, "yes") || !strcmp(t, "true"); }

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(1 << 20);
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void show(char *out, size_t cap, const char *label, const char *v)
{
    /* Escape newlines so mismatches are readable on one line. */
    size_t o = (size_t)snprintf(out, cap, "%s \"", label);
    for (const char *p = v ? v : "(null)"; *p && o + 4 < cap; p++) {
        if (*p == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (*p == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else out[o++] = *p;
    }
    /* Never end on a split multibyte sequence: the report must stay valid UTF-8
     * when a long value is cut. Walk back over continuation bytes to the lead
     * byte and drop the character if its full length did not fit. */
    size_t start = o;
    while (start > 0 && ((unsigned char)out[start - 1] & 0xC0) == 0x80 && o - start < 3) start--;
    if (start > 0 && ((unsigned char)out[start - 1] & 0xC0) == 0xC0) {
        unsigned char lead = (unsigned char)out[--start];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (o - start < need) o = start;
    }
    snprintf(out + o, cap - o, "\"");
}

static void run_expect(Script *s, ScriptHost *h, Tokens *t)
{
    char detail[1200] = "";
    bool ok = false;
    const char *what = t->n > 1 ? t->tok[1] : "";
    Editor *e = h->editor;

    if (!strcmp(what, "text") && t->n == 3) {
        char *txt = editor_text_utf8(e);
        ok = txt && !strcmp(txt, t->tok[2]);
        if (!ok) show(detail, sizeof(detail), "got", txt);
        free(txt);
    } else if (!strcmp(what, "selection") && t->n == 4) {
        size_t a, c;
        editor_get_selection(e, &a, &c);
        ok = a == strtoul(t->tok[2], NULL, 10) && c == strtoul(t->tok[3], NULL, 10);
        snprintf(detail, sizeof(detail), "got anchor %zu cursor %zu", a, c);
    } else if (!strcmp(what, "cursor") && t->n == 3) {
        size_t a, c;
        editor_get_selection(e, &a, &c);
        ok = c == strtoul(t->tok[2], NULL, 10);
        snprintf(detail, sizeof(detail), "got cursor %zu", c);
    } else if (!strcmp(what, "anchor") && t->n == 3) {
        size_t a, c;
        editor_get_selection(e, &a, &c);
        ok = a == strtoul(t->tok[2], NULL, 10);
        snprintf(detail, sizeof(detail), "got anchor %zu", a);
    } else if (!strcmp(what, "cursor_range") && t->n == 4) {
        size_t a, c;
        editor_get_selection(e, &a, &c);
        ok = c >= strtoul(t->tok[2], NULL, 10) && c <= strtoul(t->tok[3], NULL, 10);
        snprintf(detail, sizeof(detail), "got cursor %zu", c);
    } else if (!strcmp(what, "length") && t->n == 4) {
        size_t n = editor_length(e);
        ok = n >= strtoul(t->tok[2], NULL, 10) && n <= strtoul(t->tok[3], NULL, 10);
        snprintf(detail, sizeof(detail), "got length %zu", n);
    } else if (!strcmp(what, "chat_count") && t->n == 3 && h->chat) {
        chat_lock(h->chat);
        size_t n = chat_count(h->chat);
        chat_unlock(h->chat);
        ok = n == strtoul(t->tok[2], NULL, 10);
        snprintf(detail, sizeof(detail), "got %zu", n);
    } else if ((!strcmp(what, "chat_first_seq") || !strcmp(what, "chat_last_seq") || !strcmp(what, "chat_excerpt_start")) && t->n == 3 && h->chat) {
        chat_lock(h->chat);
        size_t n = chat_count(h->chat);
        const ChatMessage *m = n ? chat_message(h->chat, !strcmp(what, "chat_first_seq") ? 0 : n - 1) : NULL;
        int64_t value = m ? (!strcmp(what, "chat_excerpt_start") ? (int64_t)m->excerpt_start : m->sequence) : -1;
        ok = m && m->sequence_known && value == strtoll(t->tok[2], NULL, 10);
        snprintf(detail, sizeof(detail), "got %lld", (long long)value);
        chat_unlock(h->chat);
    } else if (!strcmp(what, "chat_cache_max") && t->n == 3 && h->history_cache_bytes) {
        size_t bytes = h->history_cache_bytes(h->ctx);
        ok = bytes <= strtoull(t->tok[2], NULL, 10);
        snprintf(detail, sizeof(detail), "got %zu cache bytes", bytes);
    } else if (!strcmp(what, "memory_enabled") && t->n == 3 && h->memory_enabled) {
        ok = h->memory_enabled(h->ctx) == yes(t->tok[2]);
    } else if (!strcmp(what, "memory_sources_none") && t->n == 2 && h->memory_sources) {
        /* No sources at all (memory off for the conversation, or no reply): distinct from "sources without X". */
        char *sources = h->memory_sources(h->ctx);
        ok = !sources || !sources[0];
        show(detail, sizeof(detail), "memory_sources", sources ? sources : "");
        free(sources);
    } else if ((!strcmp(what, "memory_sources") || !strcmp(what, "memory_sources_not")) && t->n == 3 && h->memory_sources) {
        char *sources = h->memory_sources(h->ctx);
        bool found = sources && strstr(sources, t->tok[2]);
        ok = !strcmp(what, "memory_sources") ? found : (sources && !found); /* no reply is not "sources without X" */
        show(detail, sizeof(detail), "memory_sources", sources ? sources : "");
        free(sources);
    } else if ((!strcmp(what, "recall") || !strcmp(what, "recall_not")) && t->n == 3 && h->recall_text) {
        char *text = h->recall_text(h->ctx);
        bool found = text && strstr(text, t->tok[2]);
        ok = !strcmp(what, "recall") ? found : (text && !found);
        show(detail, sizeof(detail), "recall", text ? text : "");
        free(text);
    } else if (!strcmp(what, "history_busy") && t->n == 3 && h->history_busy) {
        ok = h->history_busy(h->ctx) == yes(t->tok[2]);
    } else if (!strcmp(what, "chat_unsaved") && t->n == 3 && h->chat) {
        chat_lock(h->chat);
        ok = chat_window_info(h->chat).unsaved == yes(t->tok[2]);
        chat_unlock(h->chat);
    } else if (!strcmp(what, "chat_notes") && t->n == 3 && h->chat) { /* user turns the author marked as notes */
        chat_lock(h->chat);
        size_t n = chat_count(h->chat), notes = 0;
        for (size_t i = 0; i < n; i++) notes += chat_message(h->chat, i)->is_note;
        chat_unlock(h->chat);
        ok = notes == strtoul(t->tok[2], NULL, 10);
        snprintf(detail, sizeof(detail), "got %zu", notes);
    } else if (!strcmp(what, "chat_last") && t->n == 3 && h->chat) {
        chat_lock(h->chat);
        size_t n = chat_count(h->chat);
        const ChatMessage *m = n ? chat_message(h->chat, n - 1) : NULL;
        ok = m && strstr(m->content, t->tok[2]);
        show(detail, sizeof(detail), "last", m ? m->content : "(none)");
        chat_unlock(h->chat);
    } else if (!strcmp(what, "chat_last_not") && t->n == 3 && h->chat) {
        chat_lock(h->chat);
        size_t n = chat_count(h->chat);
        const ChatMessage *m = n ? chat_message(h->chat, n - 1) : NULL;
        ok = m && !strstr(m->content, t->tok[2]);
        show(detail, sizeof(detail), "last", m ? m->content : "(none)");
        chat_unlock(h->chat);
    } else if (!strcmp(what, "element_w") && t->n == 5 && h->element) {
        Rectangle r = h->element(h->ctx, t->tok[2]);
        ok = r.width >= strtof(t->tok[3], NULL) && r.width <= strtof(t->tok[4], NULL);
        snprintf(detail, sizeof(detail), "got %.1f", r.width);
    } else if (!strcmp(what, "element_y") && t->n == 5 && h->element) {
        /* Where a click on the element would land, after any scroll the lookup applied. */
        Rectangle r = h->element(h->ctx, t->tok[2]);
        float y = r.y + r.height / 2;
        ok = r.width > 0 && y >= strtof(t->tok[3], NULL) && y <= strtof(t->tok[4], NULL);
        snprintf(detail, sizeof(detail), "got %.1f", y);
    } else if (!strcmp(what, "chat_busy") && t->n == 3 && h->chat) {
        ok = chat_busy(h->chat) == yes(t->tok[2]);
    } else if (!strcmp(what, "dirty") && t->n == 3) {
        ok = editor_is_dirty(e) == yes(t->tok[2]);
    } else if (!strcmp(what, "readonly") && t->n == 3) {
        ok = editor_is_read_only(e) == yes(t->tok[2]);
    } else if ((!strcmp(what, "status") || !strcmp(what, "status_not")) && t->n == 3) {
        const char *st = h->status(h->ctx);
        bool has = st && strstr(st, t->tok[2]);
        ok = !strcmp(what, "status") ? has : (st && !has);
        show(detail, sizeof(detail), "status", st);
    } else if (!strcmp(what, "clipboard") && t->n == 3) {
        const char *c = input_get_clipboard();
        ok = c && !strcmp(c, t->tok[2]);
        show(detail, sizeof(detail), "clipboard", c);
    } else if (!strcmp(what, "file") && t->n == 4) {
        char *c = slurp(t->tok[2]);
        ok = c && !strcmp(c, t->tok[3]);
        show(detail, sizeof(detail), "file", c);
        free(c);
    } else if (!strcmp(what, "lines") && t->n == 5) {
        /* Lines of a file containing a substring (e.g. versions in a log). */
        char *c = slurp(t->tok[2]);
        int count = 0;
        for (char *line = c; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (strstr(line, t->tok[3])) count++;
            line = nl ? nl + 1 : NULL;
        }
        free(c);
        ok = count == atoi(t->tok[4]);
        snprintf(detail, sizeof(detail), "found %d", count);
    } else if ((!strcmp(what, "findings_has") || !strcmp(what, "findings_not")) && t->n == 3 && h->findings_text) {
        char *c = h->findings_text(h->ctx);
        bool has = c && strstr(c, t->tok[2]);
        ok = !strcmp(what, "findings_has") ? has : (c && !has);
        show(detail, sizeof(detail), "findings", c);
        free(c);
    } else if (!strcmp(what, "attached") && t->n == 3 && h->attached_text) {
        char *c = h->attached_text(h->ctx);
        ok = c && !strcmp(c, t->tok[2]);
        show(detail, sizeof(detail), "attached", c);
        free(c);
    } else if (!strcmp(what, "observer_status") && t->n == 3 && h->observer_status) {
        char *c = h->observer_status(h->ctx);
        ok = c && strstr(c, t->tok[2]);
        show(detail, sizeof(detail), "observer", c);
        free(c);
    } else if (!strcmp(what, "explorer_has") && t->n == 3 && h->explorer_text) {
        char *c = h->explorer_text(h->ctx);
        ok = false;
        for (char *save = NULL, *l = c ? strtok_r(c, "\n", &save) : NULL; l && !ok; l = strtok_r(NULL, "\n", &save))
            ok = !strcmp(l, t->tok[2]);
        snprintf(detail, sizeof(detail), "not listed");
        free(c);
    } else if (!strcmp(what, "findings_count") && t->n == 3 && h->findings_text) {
        char *c = h->findings_text(h->ctx);
        int count = 0;
        for (char *p = c; p && *p; p++) count += *p == '\n';
        ok = c && count == atoi(t->tok[2]);
        snprintf(detail, sizeof(detail), "found %d", count);
        free(c);
    } else if (!strcmp(what, "record") && t->n == 3 && h->record_text) {
        /* The open note/canon panel: its header and text ("" when closed). */
        char *c = h->record_text(h->ctx);
        ok = c && strstr(c, t->tok[2]);
        show(detail, sizeof(detail), "record", c);
        free(c);
    } else if (!strcmp(what, "files") && t->n == 5) {
        int count = 0;
        DIR *d = opendir(t->tok[2]);
        struct dirent *ent;
        while (d && (ent = readdir(d)) != NULL)
            if (strstr(ent->d_name, t->tok[3])) count++;
        if (d) closedir(d);
        ok = count == atoi(t->tok[4]);
        snprintf(detail, sizeof(detail), "found %d", count);
    } else if (!strcmp(what, "visible") && t->n == 4) {
        ok = editor_index_visible(e, strtoul(t->tok[2], NULL, 10)) == yes(t->tok[3]);
        snprintf(detail, sizeof(detail), "scroll_y %.1f", editor_scroll_y(e));
    } else if (!strcmp(what, "scrolled") && t->n == 3) {
        ok = (editor_scroll_y(e) > 0.5f) == yes(t->tok[2]);
        snprintf(detail, sizeof(detail), "scroll_y %.1f", editor_scroll_y(e));
    } else if ((!strcmp(what, "explorer_w") || !strcmp(what, "observer_w") || !strcmp(what, "editor_w")) && t->n == 4) {
        float v = !strcmp(what, "explorer_w") ? *h->explorer_w
                : !strcmp(what, "observer_w") ? *h->observer_w : h->editor_rect->width;
        ok = v >= strtof(t->tok[2], NULL) && v <= strtof(t->tok[3], NULL);
        snprintf(detail, sizeof(detail), "got %.1f", v);
    } else if (!strcmp(what, "running") && t->n == 3 && !yes(t->tok[2])) {
        /* Reached while the app is still running: the close did not exit. */
        s->expect_exit = true;
        s->expect_exit_line = s->pc;
        ok = false;
        snprintf(detail, sizeof(detail), "app is still running");
    } else {
        snprintf(detail, sizeof(detail), "malformed expect");
    }
    report(s, ok, detail);
}

/* Execute one command. Returns true if it consumed frames (queued events or
 * a wait), meaning the runner should yield. */
static bool run_line(Script *s, ScriptHost *h)
{
    static char storage[TEXT_MAX * 2];
    Tokens t;
    if (!tokenize(s, s->lines[s->pc], &t, storage, sizeof(storage))) {
        report(s, false, "parse error");
        return false;
    }
    if (t.n == 0) return false;
    const char *cmd = t.tok[0];
    Editor *e = h->editor;

    if (!strcmp(cmd, "expect")) {
        run_expect(s, h, &t);
        return false;
    }
    if (!strcmp(cmd, "open") && t.n == 2) { h->open(h->ctx, t.tok[1]); new_batch(s); return true; }
    if (!strcmp(cmd, "drop") && t.n == 2) { h->drop(h->ctx, t.tok[1]); new_batch(s); return true; }
    if (!strcmp(cmd, "type") && t.n == 2) {
        size_t len = strlen(t.tok[1]);
        Batch *b = new_batch(s);
        if (b) {
            b->chars = malloc((len + 1) * sizeof(uint32_t));
            b->nchars = utf8_decode((const uint8_t *)t.tok[1], len, b->chars, NULL);
        }
        new_batch(s);
        return true;
    }
    if (!strcmp(cmd, "key") && t.n >= 2) {
        int k = key_code(t.tok[1]);
        if (k < 0) { report(s, false, "unknown key"); return false; }
        int mods[4], nm = 0;
        for (int i = 2; i < t.n && nm < 4; i++) {
            int m = mod_code(t.tok[i]);
            if (m < 0) { report(s, false, "unknown modifier"); return false; }
            mods[nm++] = m;
        }
        Batch *down = new_batch(s);
        for (int i = 0; i < nm; i++) add_ev(down, EV_KEY_DOWN, mods[i], 0);
        add_ev(down, EV_KEY_DOWN, k, 0);
        Batch *up = new_batch(s);
        add_ev(up, EV_KEY_UP, k, 0);
        for (int i = 0; i < nm; i++) add_ev(up, EV_KEY_UP, mods[i], 0);
        return true;
    }
    if (!strcmp(cmd, "click") && t.n >= 3) {
        int count = t.n >= 4 && t.tok[3][0] != 's' ? atoi(t.tok[3]) : 1;
        bool shift = !strcmp(t.tok[t.n - 1], "shift");
        queue_click(s, atoi(t.tok[1]), atoi(t.tok[2]), count > 0 ? count : 1, shift);
        return true;
    }
    if (!strcmp(cmd, "click_index") && t.n >= 2) {
        float x, y;
        if (!editor_index_screen_pos(e, strtoul(t.tok[1], NULL, 10), &x, &y)) {
            report(s, false, "index out of range");
            return false;
        }
        int count = t.n >= 3 && t.tok[2][0] != 's' ? atoi(t.tok[2]) : 1;
        bool shift = !strcmp(t.tok[t.n - 1], "shift");
        queue_click(s, (int)lroundf(x + 1.0f), (int)lroundf(y), count > 0 ? count : 1, shift);
        return true;
    }
    if (!strcmp(cmd, "drag") && t.n >= 5) {
        queue_drag(s, atoi(t.tok[1]), atoi(t.tok[2]), atoi(t.tok[3]), atoi(t.tok[4]), t.n >= 6 ? atoi(t.tok[5]) : 0);
        return true;
    }
    if (!strcmp(cmd, "drag_index") && t.n == 3) {
        float x1, y1, x2, y2;
        if (!editor_index_screen_pos(e, strtoul(t.tok[1], NULL, 10), &x1, &y1) ||
            !editor_index_screen_pos(e, strtoul(t.tok[2], NULL, 10), &x2, &y2)) {
            report(s, false, "index out of range");
            return false;
        }
        queue_drag(s, (int)lroundf(x1 + 1), (int)lroundf(y1), (int)lroundf(x2 + 1), (int)lroundf(y2), 0);
        return true;
    }
    if (!strcmp(cmd, "move") && t.n == 3) {
        add_ev(new_batch(s), EV_MOUSE_POSITION, atoi(t.tok[1]), atoi(t.tok[2]));
        new_batch(s);
        return true;
    }
    if (!strcmp(cmd, "wheel") && t.n == 2) {
        add_ev(new_batch(s), EV_MOUSE_WHEEL, 0, atoi(t.tok[1]));
        new_batch(s);
        return true;
    }
    if (!strcmp(cmd, "measure_frames") && t.n == 2) {
        if (!strcmp(t.tok[1], "start")) {
            frame_metrics_clear(&s->frame_metrics);
            frame_metrics_record(&s->frame_metrics, s->frame_started_s);
            s->measuring_frames = true;
        } else if (!strcmp(t.tok[1], "stop")) {
            FrameMetricsReport r = frame_metrics_report(&s->frame_metrics);
            s->measuring_frames = false;
            printf("FRAME_METRICS interframe_ms count=%" PRIu64 " p50=%.3f p95=%.3f p99=%.3f max=%.3f sample_seconds=%.3f over_50ms=%" PRIu64
                   " (includes waits/vsync; not input latency, isolated CPU time, or an SLA)\n",
                   r.count, r.p50_ms, r.p95_ms, r.p99_ms, r.max_ms, r.sample_s, r.over_budget);
        } else report(s, false, "measure_frames needs start or stop");
        return false;
    }
    if (!strcmp(cmd, "wait") && t.n == 2) { s->wait = atoi(t.tok[1]); return true; }
    if (!strcmp(cmd, "wait_ready")) {
        s->wait_ready = t.n >= 2 ? atoi(t.tok[1]) : 60 * 180;
        s->pc--; /* stay on this line so it reports pass/fail when done */
        return true;
    }
    if (!strcmp(cmd, "observer") && t.n == 2 && h->observer_enable) {
        h->observer_enable(h->ctx, !strcmp(t.tok[1], "on"));
        return false;
    }
    if (!strcmp(cmd, "store_offline") && t.n == 2 && h->store_offline) {
        h->store_offline(h->ctx, yes(t.tok[1]));
        return false;
    }
    if (!strcmp(cmd, "wait_observer") && h->observer_idle) {
        s->wait_observer = t.n >= 2 ? atoi(t.tok[1]) : 60 * 60;
        s->pc--; /* reports pass/fail when done */
        return true;
    }
    /* wait_status "text" [frames]: until the status line contains text
     * (background work: backups); fails if it never does. */
    if (!strcmp(cmd, "wait_status") && t.n >= 2 && h->status) {
        snprintf(s->wait_text, sizeof(s->wait_text), "%s", t.tok[1]);
        s->wait_status = t.n >= 3 ? atoi(t.tok[2]) : 60 * 60;
        s->pc--; /* reports pass/fail when done */
        return true;
    }
    if (!strcmp(cmd, "wait_chat") && h->chat) {
        s->wait_chat = t.n >= 2 ? atoi(t.tok[1]) : 1200;
        return true;
    }
    if (!strcmp(cmd, "cancel_chat") && t.n == 1 && h->chat) {
        chat_cancel(h->chat);
        return false;
    }
    if (!strcmp(cmd, "wait_recall") && h->recall_busy) {
        s->wait_recall = t.n >= 2 ? atoi(t.tok[1]) : 1200;
        if (s->wait_recall <= 0) { report(s, false, "recall wait needs a positive frame limit"); return false; }
        s->pc--;
        return true;
    }
    if (!strcmp(cmd, "wait_history") && h->history_busy) {
        s->wait_history = t.n >= 2 ? atoi(t.tok[1]) : 1200;
        if (s->wait_history <= 0) { report(s, false, "history wait needs a positive frame limit"); return false; }
        s->pc--;
        return true;
    }
    if (!strcmp(cmd, "wait_memory_briefs") && h->memory_briefs_ready && (t.n == 2 || t.n == 3)) {
        char *end;
        long count = strtol(t.tok[1], &end, 10);
        if (!t.tok[1][0] || *end || count < 1 || count > 1000) { report(s, false, "invalid brief count"); return false; }
        long frames = 7200;
        if (t.n == 3) {
            frames = strtol(t.tok[2], &end, 10);
            if (!t.tok[2][0] || *end || frames < 1 || frames > 120000) { report(s, false, "invalid brief wait"); return false; }
        }
        s->memory_briefs_count = count; s->wait_memory_briefs = (int)frames; s->pc--;
        return true;
    }
    if (!strcmp(cmd, "wait_memory_documents") && h->memory_documents_settled && (t.n == 2 || t.n == 3)) {
        char *end;
        long count = strtol(t.tok[1], &end, 10);
        if (!t.tok[1][0] || *end || count < 0 || count > 8) { report(s, false, "invalid document count"); return false; }
        long frames = 1800;
        if (t.n == 3) {
            frames = strtol(t.tok[2], &end, 10);
            if (!t.tok[2][0] || *end || frames < 1 || frames > 120000) { report(s, false, "invalid document wait"); return false; }
        }
        s->memory_documents_count = count; s->wait_memory_documents = (int)frames; s->pc--;
        return true;
    }
    if (!strcmp(cmd, "wait_memory") && h->memory_settled && (t.n == 2 || t.n == 3)) {
        char *end;
        bool off = !strcmp(t.tok[1], "off");
        long seq = off ? -1 : strtol(t.tok[1], &end, 10);
        if (!off && (!t.tok[1][0] || *end || seq < 0 || seq > INT_MAX)) { report(s, false, "invalid memory sequence"); return false; }
        long frames = 1800;
        if (t.n == 3) {
            frames = strtol(t.tok[2], &end, 10);
            if (!t.tok[2][0] || *end || frames < 1 || frames > 120000) { report(s, false, "invalid memory wait"); return false; }
        }
        s->memory_sequence = seq; s->wait_memory = (int)frames; s->pc--;
        return true;
    }
    if (!strcmp(cmd, "history_hold") && t.n == 2 && h->history_hold) {
        h->history_hold(h->ctx, yes(t.tok[1]));
        return false;
    }
    if (!strcmp(cmd, "store_restart") && t.n == 1 && h->store_restart) {
        h->store_restart(h->ctx);
        return false;
    }
    if (!strcmp(cmd, "right_click_element") && t.n == 2 && h->element) {
        Rectangle r = h->element(h->ctx, t.tok[1]);
        if (r.width <= 0) {
            report(s, false, "no such element");
            return false;
        }
        new_batch(s); /* a lookup may have scrolled the element into view; let the layout apply it first */
        queue_click_button(s, (int)(r.x + r.width / 2), (int)(r.y + r.height / 2), 1, false, MOUSE_BUTTON_RIGHT);
        return true;
    }
    /* dialog_latest DIR: the next dialog answers with DIR's newest entry
     * (names that sort by time, as backups' do). */
    if (!strcmp(cmd, "dialog_latest") && t.n == 2) {
        DIR *d = opendir(t.tok[1]);
        struct dirent *ent;
        char best[256] = "";
        while (d && (ent = readdir(d)) != NULL)
            if (ent->d_name[0] != '.' && strcmp(ent->d_name, best) > 0) snprintf(best, sizeof(best), "%s", ent->d_name);
        if (d) closedir(d);
        char path[1400];
        snprintf(path, sizeof(path), "%s/%s", t.tok[1], best);
        if (!best[0]) report(s, false, "the folder is empty");
        dialogs_script_answer(best[0] ? path : "");
        return false;
    }
    /* click_finding TEXT: click the first finding whose line (findings_has
     * format) contains TEXT. */
    if (!strcmp(cmd, "click_finding") && t.n == 2 && h->findings_text && h->element) {
        char *c = h->findings_text(h->ctx);
        int idx = -1, i = 0;
        for (char *save = NULL, *l = c ? strtok_r(c, "\n", &save) : NULL; l && idx < 0; l = strtok_r(NULL, "\n", &save), i++)
            if (strstr(l, t.tok[1])) idx = i;
        free(c);
        char name[32];
        snprintf(name, sizeof(name), "Finding%d", idx);
        Rectangle r = idx >= 0 ? h->element(h->ctx, name) : (Rectangle){ 0 };
        if (r.width <= 0) {
            report(s, false, "no such finding");
            return false;
        }
        new_batch(s);
        queue_click_button(s, (int)(r.x + r.width / 2), (int)(r.y + 12), 1, false, MOUSE_BUTTON_LEFT);
        return true;
    }
    if (!strcmp(cmd, "dialog") && t.n == 2) {
        dialogs_script_answer(t.tok[1]); /* the next native dialog's answer ("" = Cancel) */
        return false;
    }
    if (!strcmp(cmd, "click_element") && t.n >= 2 && h->element) {
        Rectangle r = h->element(h->ctx, t.tok[1]);
        if (r.width <= 0) {
            report(s, false, "no such element");
            return false;
        }
        int count = t.n >= 3 ? atoi(t.tok[2]) : 1;
        new_batch(s); /* a lookup may have scrolled the element into view; let the layout apply it first */
        queue_click(s, (int)(r.x + r.width / 2), (int)(r.y + r.height / 2), count > 0 ? count : 1, false);
        return true;
    }
    if (!strcmp(cmd, "resize") && t.n == 3) {
        add_ev(new_batch(s), EV_WINDOW_RESIZE, atoi(t.tok[1]), atoi(t.tok[2]));
        for (int i = 0; i < 3; i++) new_batch(s); /* let the resize propagate */
        return true;
    }
    if (!strcmp(cmd, "close") && t.n == 1) {
        add_ev(new_batch(s), EV_WINDOW_CLOSE, 0, 0);
        new_batch(s);
        return true;
    }
    if (!strcmp(cmd, "shot") && t.n == 2) {
        snprintf(s->shot, sizeof(s->shot), "%s/%s.png", s->shot_dir, t.tok[1]);
        s->shot_pending = true;
        new_batch(s);
        return true;
    }
    if (!strcmp(cmd, "clipboard") && t.n == 2) { input_set_clipboard(t.tok[1]); return false; }
    if (!strcmp(cmd, "write") && t.n == 3) {
        FILE *f = fopen(t.tok[1], "wb");
        if (f) {
            fputs(t.tok[2], f);
            fclose(f);
        } else {
            report(s, false, "cannot write file");
        }
        return false;
    }
    if (!strcmp(cmd, "mkdir") && t.n == 2) {
        mkdir(t.tok[1], 0755);
        return false;
    }
    if (!strcmp(cmd, "chmod") && t.n == 3) {
        chmod(t.tok[1], (mode_t)strtoul(t.tok[2], NULL, 8));
        return false;
    }
    report(s, false, "unknown command");
    return false;
}

bool script_frame(Script *s, ScriptHost *h)
{
    /* Once per invocation, never once per command in the dispatch loop. */
    s->frame_started_s = frame_now();
    if (s->measuring_frames) frame_metrics_record(&s->frame_metrics, s->frame_started_s);
    unsigned long frame = atomic_fetch_add(&s->frames, 1);
    atomic_store(&s->line, s->pc > 0 ? s->pc - 1 : 0);
    if (frame < 2) return true; /* let the UI lay out once before the first command */
    for (;;) {
        if (s->qcount) {
            play_next_batch(s);
            return true;
        }
        if (s->wait > 0) {
            s->wait--;
            return true;
        }
        if (s->wait_ready > 0) {
            if (h->model_ready && h->model_ready(h->ctx)) {
                s->wait_ready = 0;
                report(s, true, NULL);
                s->pc++;
                continue;
            }
            if (--s->wait_ready == 0) {
                char detail[640] = "model did not become ready";
                char *why = h->model_status ? h->model_status(h->ctx) : NULL;
                if (why && *why) snprintf(detail, sizeof(detail), "model did not become ready: %s", why);
                free(why);
                report(s, false, detail);
                s->pc++;
                continue;
            }
            return true;
        }
        if (s->wait_observer > 0) {
            /* Idle on two consecutive checks a few frames apart, so a poke
             * that is about to be picked up isn't mistaken for idleness. */
            if (h->observer_idle(h->ctx) && s->wait_observer % 5 == 0) {
                s->wait_observer = 0;
                report(s, true, NULL);
                s->pc++;
                new_batch(s);
                continue;
            }
            if (--s->wait_observer == 0) {
                report(s, false, "observer did not finish");
                s->pc++;
                continue;
            }
            return true;
        }
        if (s->wait_status > 0) {
            const char *st = h->status(h->ctx);
            bool found = st && strstr(st, s->wait_text);
            if (found || --s->wait_status == 0) {
                s->wait_status = 0;
                report(s, found, found ? NULL : "status never showed it");
                s->pc++;
                new_batch(s);
                continue;
            }
            return true;
        }
        if (s->wait_memory_briefs > 0) {
            bool done = h->memory_briefs_ready(h->ctx, s->memory_briefs_count);
            if (done || --s->wait_memory_briefs == 0) {
                s->wait_memory_briefs = 0;
                report(s, done, done ? NULL : "source-linked T1 brief did not publish");
                s->pc++; new_batch(s); continue;
            }
            return true;
        }
        if (s->wait_memory_documents > 0) {
            bool done = h->memory_documents_settled(h->ctx, s->memory_documents_count);
            if (done || --s->wait_memory_documents == 0) {
                s->wait_memory_documents = 0;
                report(s, done, done ? NULL : "saved document ingestion/outbox did not settle");
                s->pc++; new_batch(s); continue;
            }
            return true;
        }
        if (s->wait_memory > 0) {
            bool done = h->memory_settled(h->ctx, s->memory_sequence);
            if (done || --s->wait_memory == 0) {
                s->wait_memory = 0;
                report(s, done, done ? NULL : "memory ingestion/outbox did not settle");
                s->pc++; new_batch(s); continue;
            }
            return true;
        }
        if (s->wait_recall > 0) {
            bool done = !h->recall_busy(h->ctx);
            if (done || --s->wait_recall == 0) {
                s->wait_recall = 0;
                report(s, done, done ? NULL : "recall did not finish");
                s->pc++;
                new_batch(s);
                continue;
            }
            return true;
        }
        if (s->wait_history > 0) {
            bool done = !h->history_busy(h->ctx) && (!h->history_worker_busy || !h->history_worker_busy(h->ctx));
            if (done || --s->wait_history == 0) {
                s->wait_history = 0;
                report(s, done, done ? NULL : "history load did not finish");
                s->pc++;
                new_batch(s);
                continue;
            }
            return true;
        }
        if (s->wait_chat > 0) {
            s->wait_chat--;
            if (chat_busy(h->chat) && s->wait_chat > 0) return true;
            bool done = !chat_busy(h->chat);
            s->wait_chat = 0;
            if (!done) report(s, false, "chat did not finish"); /* a hung send is a failure, not a pass-through (red-team 2026-11-14) */
            new_batch(s); /* one more frame so chat_poll publishes completion */
            continue;
        }
        if (s->pc >= s->nlines) return false;
        bool yielded = run_line(s, h);
        s->pc++;
        if (s->expect_exit) return false;
        if (yielded && !s->qcount) return true; /* a wait */
    }
}

const char *script_take_shot(Script *s)
{
    if (!s->shot_pending) return NULL;
    s->shot_pending = false;
    return s->shot;
}

int script_finish(Script *s, ScriptHost *h, bool app_closed)
{
    (void)h;
    if (app_closed) {
        /* The only commands that may follow an exit are "expect running no"
         * (which passes) and comments/blank lines. */
        for (; s->pc < s->nlines; s->pc++) {
            static char storage[TEXT_MAX * 2];
            Tokens t;
            if (!tokenize(s, s->lines[s->pc], &t, storage, sizeof(storage)) || t.n == 0) continue;
            bool ok = t.n == 3 && !strcmp(t.tok[0], "expect") && !strcmp(t.tok[1], "running") && !yes(t.tok[2]);
            report(s, ok, ok ? NULL : "app exited before this command");
        }
    } else if (s->pc < s->nlines && !s->expect_exit) {
        report(s, false, "script did not finish");
    }
    printf("  %d passed, %d failed\n", s->passed, s->failed);
    return s->failed ? 1 : 0;
}
