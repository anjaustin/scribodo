/* Diagnostic: the observer's judge alone, over a file of canon/statement pairs, with the pinned local model (the app's
 * default configuration, as observer_replay --real starts it), so a change to what the judge is shown is measured on
 * the real model without a whole replay (GAP-001; record-model-replies-before-tuning).
 *
 * usage: observer_judge_pairs IN.jsonl OUT.jsonl RUN_DIR
 *        IN: one JSON object per line with "canon" and "statement" (other fields are copied through);
 *        OUT: the same object plus "replies" (both passes' raw replies, the second null when the first was not a
 *        conflict) and "conflict" (the two-vote verdict, observer_model_judge); RUN_DIR holds the server's run folder,
 *        log and hash cache. A line already in OUT (same position) is not judged again, so a stopped run resumes.
 * stderr: "# N pairs, C conflicts, T s". Exit 0 when every pair was judged. */
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/json.h"
#include "llm/observer_model_internal.h"
#include "llm/server.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: observer_judge_pairs IN.jsonl OUT.jsonl RUN_DIR\n"); return 1; }
    FILE *in = fopen(argv[1], "r");
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    size_t done = 0;
    FILE *prev = fopen(argv[2], "r");
    if (prev) {
        for (int ch; (ch = fgetc(prev)) != EOF;) done += ch == '\n';
        fclose(prev);
    }
    FILE *out = fopen(argv[2], "a");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); fclose(in); return 1; }

    char self[PATH_MAX] = "", app_dir[PATH_MAX] = "";
    if (realpath(argv[0], self)) {
        char *slash = strrchr(self, '/');
        if (slash) snprintf(app_dir, sizeof(app_dir), "%.*s/", (int)(slash - self), self);
    }
    LlmServerConfig cfg;
    llm_server_default_config(&cfg, app_dir);
    mkdir(argv[3], 0700);
    snprintf(cfg.run_dir, sizeof(cfg.run_dir), "%s", argv[3]);
    snprintf(cfg.log_path, sizeof(cfg.log_path), "%s/server.log", argv[3]);
    snprintf(cfg.cache_path, sizeof(cfg.cache_path), "%s/model-verified.txt", argv[3]);
    cfg.ctx_total = 16384;
    cfg.slots = 2;
    LlmServer *server = llm_server_create(&cfg);
    if (!server) { fprintf(stderr, "no model server\n"); return 1; }
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
        return 1;
    }
    fprintf(stderr, "# model ready: slot context %d, %s\n", llm_server_slot_ctx(server), cfg.sha256);

    atomic_bool cancel = false;
    size_t n = 0, conflicts = 0, failed = 0;
    double t0 = now_s();
    char *line = NULL;
    size_t cap = 0;
    for (ssize_t len; (len = getline(&line, &cap, in)) > 0; n++) {
        if (n < done) continue;
        cJSON *j = json_parse(line);
        cJSON *canon = j ? cJSON_GetObjectItem(j, "canon") : NULL, *stmt = j ? cJSON_GetObjectItem(j, "statement") : NULL;
        if (!cJSON_IsString(canon) || !cJSON_IsString(stmt)) {
            fprintf(stderr, "line %zu: no canon or statement\n", n + 1);
            cJSON_Delete(j);
            failed++;
            break;
        }
        ObserverModel model = { server, &cancel, true, { NULL, NULL } };
        char check[600];
        bool conflict = observer_model_judge(&model, canon->valuestring, stmt->valuestring, check, sizeof(check));
        cJSON *rs = cJSON_AddArrayToObject(j, "replies");
        for (int p = 0; p < 2; p++) {
            cJSON_AddItemToArray(rs, model.replies[p] ? cJSON_CreateString(model.replies[p]) : cJSON_CreateNull());
            free(model.replies[p]);
        }
        cJSON_AddBoolToObject(j, "conflict", conflict);
        char *o = cJSON_PrintUnformatted(j);
        if (o) { fprintf(out, "%s\n", o); fflush(out); free(o); }
        conflicts += conflict;
        cJSON_Delete(j);
        llm_server_tick(server);
    }
    free(line);
    fclose(in);
    fclose(out);
    fprintf(stderr, "# %zu pairs, %zu conflicts (this run), %.0f s\n", n, conflicts, now_s() - t0);
    llm_server_destroy(server);
    return failed ? 2 : 0;
}
