/* Diagnostic (AUDIT-004 F12): the wall time of one draft write on the frame thread at whole-manuscript scale.
 *
 *   build/draft_tick_probe [file ...]     (no file: a synthetic 1.2 MB buffer)
 *
 * Loads each file into the real Editor (headless raylib stubs), then for N ticks changes one character (untimed)
 * and times draft_controller_tick exactly as main.c calls it. The frame thread's share is the snapshot
 * (editor_text_utf8), the FNV-1a change check and starting the writer; the write (doc_write_draft: write + fsync +
 * parent fsync) is timed separately, as the wait for the worker. With --inline the write is timed on the calling
 * thread, as main.c did before the worker (the measurement that moved it). Prints the median, p90 and worst against
 * the 50 ms worst-frame budget (MEMORY-019 "Budgets"). */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app/draft_controller.h"
#include "core/doc_io.h"
#include "ui/editor.h"
#include "ui/font.h"

#define TICKS 40

static SFont g_font;
static double g_text_ms;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static char *timed_text(void *ctx)
{
    double t0 = now_ms();
    char *s = editor_text_utf8((const Editor *)ctx);
    g_text_ms = now_ms() - t0;
    return s;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static bool g_inline;

static int probe(const char *source, const char *work_dir)
{
    char doc[1024], err[512];
    snprintf(doc, sizeof(doc), "%s/manuscript.md", work_dir);
    if (source) {
        FILE *in = fopen(source, "rb"), *out = fopen(doc, "wb");
        if (!in || !out) { perror(source); return 1; }
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(in);
        fclose(out);
    } else {
        FILE *out = fopen(doc, "wb");
        if (!out) { perror(doc); return 1; }
        static const char para[] = "It was a bright cold day in April, and the clocks were striking thirteen. "
                                   "Winston Smith, his chin nuzzled into his breast, slipped quickly through the doors.\n\n";
        for (size_t written = 0; written < 1200 * 1024; written += sizeof(para) - 1) fputs(para, out);
        fclose(out);
    }
    Editor *e = editor_create(&g_font);
    if (!e || !editor_load_file(e, doc, err, sizeof(err))) { fprintf(stderr, "load: %s\n", err); return 1; }
    char *probe_text = editor_text_utf8(e);
    size_t bytes = probe_text ? strlen(probe_text) : 0;
    free(probe_text);
    DraftController d;
    draft_controller_init(&d);
    double total[TICKS], text_ms[TICKS], hash_ms[TICKS], write_ms[TICKS];
    double clock = 0;
    for (int i = 0; i < TICKS; i++) {
        char c[2] = { (char)('a' + i % 26), 0 };
        editor_replace_range_utf8(e, (size_t)i, 1, c); /* typing between ticks: untimed */
        clock += DRAFT_INTERVAL_S;
        double t0 = now_ms();
        bool wrote = draft_controller_tick(&d, doc, true, clock, timed_text, e);
        double t1 = now_ms();
        draft_controller_wait(&d);
        write_ms[i] = now_ms() - t1;
        total[i] = (t1 - t0) + (g_inline ? write_ms[i] : 0);
        text_ms[i] = g_text_ms;
        if (!wrote || !d.path[0]) { fprintf(stderr, "tick %d wrote no draft\n", i); return 1; }
        char *s = editor_text_utf8(e);
        double h0 = now_ms();
        volatile uint64_t h = draft_text_hash(s);
        (void)h;
        hash_ms[i] = now_ms() - h0;
        free(s);
    }
    int worst = 0;
    for (int i = 1; i < TICKS; i++) if (total[i] > total[worst]) worst = i;
    double sorted[TICKS];
    memcpy(sorted, total, sizeof(sorted));
    qsort(sorted, TICKS, sizeof(double), cmp_double);
    double wsorted[TICKS];
    memcpy(wsorted, write_ms, sizeof(wsorted));
    qsort(wsorted, TICKS, sizeof(double), cmp_double);
    const char *name = source ? strrchr(source, '/') : NULL;
    printf("%-22s %8zu B  %s frame: median %6.2f p90 %6.2f worst %6.2f ms (text %4.2f, hash %4.2f)  "
           "write: median %6.2f worst %6.2f ms  budget 50 ms\n",
           name ? name + 1 : source ? source : "synthetic", bytes, g_inline ? "inline" : "worker",
           sorted[TICKS / 2], sorted[TICKS * 9 / 10], total[worst], text_ms[worst], hash_ms[worst],
           wsorted[TICKS / 2], wsorted[TICKS - 1]);
    draft_controller_discard(&d);
    draft_controller_destroy(&d);
    editor_destroy(e);
    unlink(doc);
    return 0;
}

int main(int argc, char **argv)
{
    char dir[512];
    const char *base = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/scribodo-draft-probe-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    int rc = 0, first = 1;
    if (argc > 1 && !strcmp(argv[1], "--inline")) {
        g_inline = true;
        first = 2;
    }
    if (argc <= first) rc = probe(NULL, dir);
    for (int i = first; i < argc && !rc; i++) rc = probe(argv[i], dir);
    rmdir(dir);
    return rc;
}
