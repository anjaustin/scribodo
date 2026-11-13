/* Diagnostic: the observer, headless, over one project folder with the deterministic fake backend (no model server, no
 * semantic index), run until it reports "Up to date"; then every finding it recorded, one line each, sorted, without
 * the timestamped ids, so two builds can be compared byte for byte on the same manuscript. The folder is written to
 * (.scribodo/ records and observer state), so run it on a copy.
 *
 * usage: observer_replay <project-dir> [seconds] [--canon TEXT]... [--note TEXT]... [--chat TEXT]... [--real]
 *        --canon and --note seed committed canon and active notes before the run (the conflict and overlap paths);
 *        --chat appends an author message to one chat, "20261112-120000-ab12" (the chat job path);
 *        --real starts the pinned local model (the app's default configuration, 16K context over two slots, its run
 *        folder, log and hash cache under <project-dir>/.scribodo/replay-run) and the observer reads with it instead
 *        of the fake backend, so the real model's findings over a manuscript can be measured headlessly (GAP-001).
 *        The lines are then not byte-comparable between runs; the counts and the per-type sets are the measurement.
 * stdout: one line per finding: type|subject|relation|object|status|content|source kind|doc or chat|start|len|quote|related text
 *         then "# N findings, J jobs" on stderr. Exit 0 when the observer finished, 2 when it did not within the limit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/chat_store.h"
#include "app/records.h"
#include "app/store.h"
#include "llm/observer.h"
#include "llm/server.h"
#include <sys/stat.h>
#include <limits.h>

static int cmp_lines(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *line_for(const RecordVersion *v)
{
    const SourceRef *s = v->nsources ? &v->sources[0] : NULL;
    const char *src_kind = !s ? "" : s->kind == SRC_DOC ? "doc" : "chat";
    const char *src_name = !s ? "" : s->kind == SRC_DOC ? s->doc : s->chat;
    size_t cap = 2048 + strlen(v->content) + (s && s->quote ? strlen(s->quote) : 0) +
                 (v->finding.related_text ? strlen(v->finding.related_text) : 0);
    char *line = malloc(cap);
    if (!line) return NULL;
    snprintf(line, cap, "%s|%s|%s|%s|%s|%s|%s|%s|%zu|%zu|%s|%s", v->finding.type, v->finding.subject, v->finding.relation,
             v->finding.object, v->status, v->content, src_kind, src_name, s ? s->start : 0, s ? s->len : 0,
             s && s->quote ? s->quote : "", v->finding.related_text ? v->finding.related_text : "");
    return line;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: observer_replay <project-dir> [seconds]\n"); return 1; }
    const char *project = argv[1];
    double limit = argc > 2 && argv[2][0] != '-' ? atof(argv[2]) : 600.0;
    StoreManager *store = store_manager_create();
    if (!store) { fprintf(stderr, "no store\n"); return 1; }
    for (int a = 2; a + 1 < argc; a++) {
        bool canon = !strcmp(argv[a], "--canon"), note = !strcmp(argv[a], "--note"), chat = !strcmp(argv[a], "--chat");
        if (!canon && !note && !chat) continue;
        bool ok;
        if (chat) {
            ChatMessage m = { .role = CHAT_USER, .content = argv[a + 1] };
            ok = chat_store_append(store, project, "20261112-120000-ab12", "replay", &m);
        } else {
            char id[40];
            records_new_id(canon ? REC_CANON : REC_NOTE, id, sizeof(id));
            ok = records_append(store, project, canon ? REC_CANON : REC_NOTE, id, canon ? "committed" : "active", argv[a + 1], NULL, 0, NULL);
        }
        if (!ok) { fprintf(stderr, "could not seed %s\n", argv[a]); store_manager_destroy(store); return 1; }
        a++;
    }
    LlmServer *server = NULL;
    bool real = false;
    for (int a = 2; a < argc; a++) real = real || !strcmp(argv[a], "--real");
    if (real) {
        /* The app's layout relative to this binary (build/observer_replay beside build/llama/bin/llama-server). */
        char self[PATH_MAX] = "", app_dir[PATH_MAX] = "";
        if (realpath(argv[0], self)) {
            char *slash = strrchr(self, '/');
            if (slash) snprintf(app_dir, sizeof(app_dir), "%.*s/", (int)(slash - self), self);
        }
        LlmServerConfig cfg;
        llm_server_default_config(&cfg, app_dir);
        char run[1024];
        snprintf(run, sizeof(run), "%s/.scribodo", project);
        mkdir(run, 0700);
        snprintf(run, sizeof(run), "%s/.scribodo/replay-run", project);
        mkdir(run, 0700);
        snprintf(cfg.run_dir, sizeof(cfg.run_dir), "%s", run);
        snprintf(cfg.log_path, sizeof(cfg.log_path), "%s/server.log", run);
        snprintf(cfg.cache_path, sizeof(cfg.cache_path), "%s/model-verified.txt", run);
        cfg.ctx_total = 16384;
        cfg.slots = 2;
        server = llm_server_create(&cfg);
        if (!server) { fprintf(stderr, "no model server\n"); store_manager_destroy(store); return 1; }
        llm_server_start(server);
        for (int i = 0; i < 180 * 20; i++) {
            LlmState st = llm_server_state(server, NULL, 0);
            if (st == LLM_READY || st == LLM_FAILED) break;
            usleep(50000);
            llm_server_tick(server);
        }
        char why[200] = "";
        if (llm_server_state(server, why, sizeof(why)) != LLM_READY) {
            fprintf(stderr, "model did not become ready: %s\n", why);
            llm_server_destroy(server);
            store_manager_destroy(store);
            return 1;
        }
        fprintf(stderr, "# model ready: slot context %d, %s\n", llm_server_slot_ctx(server), cfg.sha256);
    }
    Observer *o = observer_create(store, server, NULL);
    if (!o) { fprintf(stderr, "no observer\n"); if (server) llm_server_destroy(server); store_manager_destroy(store); return 1; }
    observer_set_project(o, project);
    observer_poke(o);
    bool finished = false;
    char status[200] = "";
    for (int i = 0; i < (int)(limit * 20) && !finished; i++) {
        usleep(50000);
        if (server) llm_server_tick(server);
        observer_status(o, status, sizeof(status));
        finished = !strcmp(status, "Up to date");
    }
    unsigned jobs = observer_jobs_done(o);
    observer_destroy(o);
    if (server) llm_server_destroy(server);
    Record *list = NULL;
    size_t n = 0, nlines = 0;
    records_list_checked(store, project, REC_FINDING, &list, &n);
    char **lines = calloc(n ? n : 1, sizeof(char *));
    for (size_t i = 0; lines && i < n; i++) {
        char *l = line_for(record_current(&list[i]));
        if (l) lines[nlines++] = l;
    }
    if (lines) qsort(lines, nlines, sizeof(char *), cmp_lines);
    for (size_t i = 0; lines && i < nlines; i++) { puts(lines[i]); free(lines[i]); }
    free(lines);
    records_free(list, n);
    store_manager_destroy(store);
    fprintf(stderr, "# %zu findings, %u jobs, status: %s\n", nlines, jobs, status);
    return finished ? 0 : 2;
}
