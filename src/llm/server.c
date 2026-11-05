#define _DARWIN_C_SOURCE
#include "server.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "llm/http.h"

extern char **environ;

/* Pinned artifact (docs/MODEL-001). */
#define LFM3_GGUF_REV "cd0c74c575b5c416f3011b5b4c298077f26ca8c2"
#define LFM3_GGUF_SHA256 "39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68"
#define MAX_RESTARTS 5      /* within one crash burst */
#define HEALTHY_RESET_S 60  /* READY this long resets the restart budget */
#define READY_TIMEOUT_S 180

struct LlmServer {
    LlmServerConfig cfg;
    pthread_mutex_t mu;
    LlmState state;
    char msg[512];
    char sock[600];
    char key[80];
    char keyfile[600];
    char pidfile[600];
    char lockfile[600];
    int lock_fd;            /* flock held while this window owns the server */
    double ready_since;
    pid_t pid;
    pthread_t thread;
    bool thread_running;
    atomic_bool stopping;
    int restarts;
    double restart_at;
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_state(LlmServer *s, LlmState st, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_state(LlmServer *s, LlmState st, const char *fmt, ...)
{
    pthread_mutex_lock(&s->mu);
    s->state = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->msg, sizeof(s->msg), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&s->mu);
}

static bool mkdir_p(const char *path, mode_t mode)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return false;
    return true;
}

#define QWEN3_EMBED_SHA256 "421a27e58d165478cc7acb984a688c2aa41404968b0203e7cd743ece44c54340"

void llm_server_embed_config(LlmServerConfig *c, const char *app_dir)
{
    llm_server_default_config(c, app_dir);
    const char *home = getenv("HOME");
    const char *model = getenv("SCRIBODO_EMBED_MODEL");
    if (model) snprintf(c->model_path, sizeof(c->model_path), "%s", model);
    else snprintf(c->model_path, sizeof(c->model_path), "%s/Library/Application Support/Scribodo/models/Qwen3-Embedding-0.6B-f16.gguf",
                  home ? home : "");
    snprintf(c->sha256, sizeof(c->sha256), "%s", QWEN3_EMBED_SHA256);
    if (home) {
        snprintf(c->run_dir, sizeof(c->run_dir), "%s/Library/Application Support/Scribodo/run-embed", home);
        snprintf(c->log_path, sizeof(c->log_path), "%s/Library/Logs/Scribodo/embed-server.log", home);
        snprintf(c->cache_path, sizeof(c->cache_path), "%s/Library/Application Support/Scribodo/embed-verified.txt", home);
    }
    /* Sized to the largest input (semantic_embed caps texts at
     * SEM_MAX_INPUT_BYTES; a token is at least one byte, plus the query
     * prefix). With flash attention off the compute buffer grows with
     * micro-batch x context: 8192 x 8192 reserved 4.7 GB of Metal memory
     * (measured 2026-11-04); 2304 x 2304 needs a sixteenth of that. */
    c->ctx_total = 2304;
    c->slots = 1;
    c->batch = 2304;
    c->embedding = true;
    /* Measured at this size: KV 252 MiB, Metal compute 443 MiB, CPU
     * compute 148 MiB, plus the process and Metal's own (about 150 MiB). */
    c->mem_fixed = 1024LL * 1024 * 1024;
    c->mem_per_token = 0; /* the context is fixed */
}

void llm_server_default_config(LlmServerConfig *c, const char *app_dir)
{
    memset(c, 0, sizeof(*c));
    const char *home = getenv("HOME");
    const char *bin = getenv("SCRIBODO_LLAMA_SERVER");
    if (bin) snprintf(c->server_bin, sizeof(c->server_bin), "%s", bin);
    else snprintf(c->server_bin, sizeof(c->server_bin), "%sllama/bin/llama-server", app_dir ? app_dir : "");
    const char *model = getenv("SCRIBODO_MODEL");
    if (model) {
        snprintf(c->model_path, sizeof(c->model_path), "%s", model);
    } else if (home) {
        /* Prefer an existing Hugging Face cache copy; else `make model`'s location. */
        snprintf(c->model_path, sizeof(c->model_path),
                 "%s/.cache/huggingface/hub/models--LiquidAI--LFM3-1.7B-GGUF/snapshots/%s/LFM3-1.7B-BF16.gguf",
                 home, LFM3_GGUF_REV);
        if (access(c->model_path, R_OK) != 0)
            snprintf(c->model_path, sizeof(c->model_path),
                     "%s/Library/Application Support/Scribodo/models/LFM3-1.7B-BF16.gguf", home);
    }
    snprintf(c->sha256, sizeof(c->sha256), "%s", LFM3_GGUF_SHA256);
    if (home) {
        snprintf(c->run_dir, sizeof(c->run_dir), "%s/Library/Application Support/Scribodo/run", home);
        snprintf(c->log_path, sizeof(c->log_path), "%s/Library/Logs/Scribodo/llama-server.log", home);
        snprintf(c->cache_path, sizeof(c->cache_path), "%s/Library/Application Support/Scribodo/model-verified.txt", home);
    }
    /* 32K context per slot, 2 slots (chat + observer), batch 256: see
     * MODEL-001 "Memory" and "Chat and observer together". */
    c->ctx_total = 65536;
    c->slots = 2;
    c->batch = 256;
    /* llama-server's memory breakdown at -c 4096/16384/32768/65536
     * (2026-11-04): KV 12 KiB per token (6 attention layers, BF16), Metal
     * compute 8.5 KiB and CPU compute 1 KiB per token (flash attention
     * off: micro-batch x context); fixed about 45 MiB (the short KV of the
     * convolution layers, compute intercepts) plus the process and Metal's
     * own. Rounded up. */
    c->mem_fixed = 384LL * 1024 * 1024;
    c->mem_per_token = 22 * 1024;
}

LlmServer *llm_server_create(const LlmServerConfig *cfg)
{
    LlmServer *s = calloc(1, sizeof(LlmServer));
    if (!s) return NULL;
    s->cfg = *cfg;
    pthread_mutex_init(&s->mu, NULL);
    atomic_init(&s->stopping, false);
    snprintf(s->sock, sizeof(s->sock), "%s/llama.sock", cfg->run_dir);
    snprintf(s->keyfile, sizeof(s->keyfile), "%s/api.key", cfg->run_dir);
    snprintf(s->pidfile, sizeof(s->pidfile), "%s/llama-server.pid", cfg->run_dir);
    snprintf(s->lockfile, sizeof(s->lockfile), "%s/server.lock", cfg->run_dir);
    s->lock_fd = -1;
    snprintf(s->msg, sizeof(s->msg), "Model not started");
    return s;
}

/* ---- model verification ------------------------------------------------ */

static bool cache_lookup(const LlmServer *s, const struct stat *st)
{
    FILE *f = fopen(s->cfg.cache_path, "r");
    if (!f) return false;
    char line[2048];
    bool hit = false;
    while (fgets(line, sizeof(line), f)) {
        char path[1100], sha[80];
        long long size, mt, ino;
        if (sscanf(line, "%1099[^\t]\t%lld\t%lld\t%lld\t%79s", path, &size, &mt, &ino, sha) == 5 &&
            !strcmp(path, s->cfg.model_path) && size == (long long)st->st_size &&
            mt == (long long)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec &&
            ino == (long long)st->st_ino && !strcmp(sha, s->cfg.sha256)) {
            hit = true;
            break;
        }
    }
    fclose(f);
    return hit;
}

static void cache_store(const LlmServer *s, const struct stat *st)
{
    FILE *f = fopen(s->cfg.cache_path, "w");
    if (!f) return;
    fprintf(f, "%s\t%lld\t%lld\t%lld\t%s\n", s->cfg.model_path, (long long)st->st_size,
            (long long)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec,
            (long long)st->st_ino, s->cfg.sha256);
    fclose(f);
}

static bool verify_model(LlmServer *s)
{
    struct stat st;
    if (!s->cfg.model_path[0] || stat(s->cfg.model_path, &st) != 0) {
        set_state(s, LLM_FAILED, "Model not found: %s (run `make model`)", s->cfg.model_path);
        return false;
    }
    if (!s->cfg.sha256[0] || cache_lookup(s, &st)) return true;

    set_state(s, LLM_VERIFYING, "Verifying model file (one-time check)\xE2\x80\xA6");
    int fd = open(s->cfg.model_path, O_RDONLY);
    if (fd < 0) {
        set_state(s, LLM_FAILED, "Cannot read model: %s", strerror(errno));
        return false;
    }
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    static char buf[1 << 22];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (atomic_load(&s->stopping)) {
            close(fd);
            return false;
        }
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    }
    close(fd);
    unsigned char dig[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(dig, &ctx);
    char hex[65];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", dig[i]);
    if (strcmp(hex, s->cfg.sha256) != 0) {
        /* Precision gate: anything but the pinned BF16 file is refused. */
        set_state(s, LLM_FAILED, "Model file does not match the pinned %s GGUF (sha256 %.12s\xE2\x80\xA6)",
                  s->cfg.embedding ? "embedding" : "BF16", hex);
        return false;
    }
    cache_store(s, &st);
    return true;
}

/* ---- process management ------------------------------------------------ */

/* One window owns the model server at a time (red-team 003: a second
 * window used to kill the first one's server as an "orphan"). The lock is
 * released automatically if this process dies, so a crashed owner's server
 * really is an orphan once we hold it. */
static bool acquire_instance_lock(LlmServer *s)
{
    if (s->lock_fd >= 0) return true;
    int fd = open(s->lockfile, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    s->lock_fd = fd;
    return true;
}

static void release_instance_lock(LlmServer *s)
{
    if (s->lock_fd >= 0) {
        flock(s->lock_fd, LOCK_UN);
        close(s->lock_fd);
        s->lock_fd = -1;
    }
}

static void kill_orphan(LlmServer *s)
{
    FILE *f = fopen(s->pidfile, "r");
    if (!f) return;
    int pid = 0;
    if (fscanf(f, "%d", &pid) == 1 && pid > 1 && kill(pid, 0) == 0) {
        /* Only kill it if it is *our* llama-server: pids get reused, so the
         * command line must name this run directory's socket. */
        char cmd[128], out[4096] = "";
        snprintf(cmd, sizeof(cmd), "ps -p %d -o args=", pid);
        FILE *p = popen(cmd, "r");
        if (p) {
            if (!fgets(out, sizeof(out), p)) out[0] = '\0';
            pclose(p);
        }
        if (strstr(out, "llama-server") && strstr(out, s->sock)) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 50 && kill(pid, 0) == 0; i++) usleep(100000);
            if (kill(pid, 0) == 0) kill(pid, SIGKILL);
        }
    }
    fclose(f);
    unlink(s->pidfile);
}

static bool write_key(LlmServer *s)
{
    unsigned char raw[24];
    arc4random_buf(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) snprintf(s->key + 2 * i, 3, "%02x", raw[i]);
    int fd = open(s->keyfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    fchmod(fd, 0600);
    bool ok = write(fd, s->key, strlen(s->key)) == (ssize_t)strlen(s->key) && write(fd, "\n", 1) == 1;
    close(fd);
    return ok;
}

static bool spawn_server(LlmServer *s)
{
    char log_dir[1024];
    snprintf(log_dir, sizeof(log_dir), "%s", s->cfg.log_path);
    char *slash = strrchr(log_dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(log_dir, 0755);
    }
    if (!mkdir_p(s->cfg.run_dir, 0700) || chmod(s->cfg.run_dir, 0700) != 0) {
        set_state(s, LLM_FAILED, "Cannot create %s", s->cfg.run_dir);
        return false;
    }
    if (!acquire_instance_lock(s)) {
        set_state(s, LLM_FAILED, "Another Scribodo window is using the model. Close it to use the model here.");
        return false;
    }
    kill_orphan(s);
    unlink(s->sock); /* a crashed server leaves its socket behind */
    if (!write_key(s)) {
        set_state(s, LLM_FAILED, "Cannot write API key file");
        return false;
    }
    if (access(s->cfg.server_bin, X_OK) != 0) {
        set_state(s, LLM_FAILED, "llama-server not found at %s (run `make llama`)", s->cfg.server_bin);
        return false;
    }

    char ctx[16], np[16], batch[16];
    snprintf(ctx, sizeof(ctx), "%d", s->cfg.ctx_total);
    snprintf(np, sizeof(np), "%d", s->cfg.slots);
    snprintf(batch, sizeof(batch), "%d", s->cfg.batch);
    /* Validated BF16 configuration (MODEL-001): flash attention OFF (Metal
     * FA stages values in F16), BF16 KV cache, official template. */
    char *chat_argv[] = {
        s->cfg.server_bin, "-m", s->cfg.model_path, "--host", s->sock, "--api-key-file", s->keyfile,
        "-ngl", "99", "-fa", "off", "-ctk", "bf16", "-ctv", "bf16", "--jinja",
        "--reasoning-format", "deepseek", "-np", np, "-c", ctx, "-b", batch, "-ub", batch,
        "--cache-ram", "0", "--no-webui", "--no-slots", NULL,
    };
    /* Embeddings: Qwen3-Embedding pools the last token; a whole input must
     * fit in one micro-batch. */
    char *embed_argv[] = {
        s->cfg.server_bin, "-m", s->cfg.model_path, "--host", s->sock, "--api-key-file", s->keyfile,
        "-ngl", "99", "-fa", "off", "--embedding", "--pooling", "last", "-np", np, "-c", ctx, "-b", batch, "-ub", batch,
        "--cache-ram", "0", "--no-webui", "--no-slots", NULL,
    };
    char **argv = s->cfg.embedding ? embed_argv : chat_argv;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, s->cfg.log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid;
    int rc = posix_spawn(&pid, s->cfg.server_bin, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        set_state(s, LLM_FAILED, "Cannot start llama-server: %s", strerror(rc));
        return false;
    }
    pthread_mutex_lock(&s->mu);
    s->pid = pid;
    pthread_mutex_unlock(&s->mu);
    FILE *f = fopen(s->pidfile, "w");
    if (f) {
        fprintf(f, "%d\n", pid);
        fclose(f);
    }
    return true;
}

static bool child_alive(LlmServer *s)
{
    pthread_mutex_lock(&s->mu);
    pid_t pid = s->pid;
    pthread_mutex_unlock(&s->mu);
    if (pid <= 0) return false;
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0) return true;
    pthread_mutex_lock(&s->mu);
    s->pid = 0;
    pthread_mutex_unlock(&s->mu);
    return false;
}

static void *start_thread(void *arg)
{
    LlmServer *s = arg;
    if (!verify_model(s)) return NULL;
    if (atomic_load(&s->stopping)) return NULL;
    set_state(s, LLM_STARTING, s->cfg.embedding ? "Loading Qwen3-Embedding\xE2\x80\xA6" : "Loading LFM3-1.7B (BF16)\xE2\x80\xA6");
    if (!spawn_server(s)) return NULL;
    double t0 = now_s();
    while (!atomic_load(&s->stopping)) {
        if (!child_alive(s)) {
            set_state(s, LLM_RESTARTING, "llama-server exited during start-up (see %s)", s->cfg.log_path);
            return NULL;
        }
        HttpResult r;
        if (http_request(s->sock, "GET", "/health", NULL, NULL, NULL, NULL, NULL, 1000, &r) && r.status == 200) {
            http_result_free(&r);
            set_state(s, LLM_READY, s->cfg.embedding ? "Qwen3-Embedding \xC2\xB7 local" : "LFM3-1.7B \xC2\xB7 BF16 \xC2\xB7 local");
            s->ready_since = now_s();
            return NULL;
        }
        http_result_free(&r);
        if (now_s() - t0 > READY_TIMEOUT_S) {
            /* Don't leave a stuck server holding ~4 GB. */
            pthread_mutex_lock(&s->mu);
            pid_t pid = s->pid;
            s->pid = 0;
            pthread_mutex_unlock(&s->mu);
            if (pid > 0) {
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
            }
            set_state(s, LLM_FAILED, "llama-server did not become ready (see %s)", s->cfg.log_path);
            return NULL;
        }
        usleep(250000);
    }
    return NULL;
}

static void join_thread(LlmServer *s)
{
    if (s->thread_running) {
        pthread_join(s->thread, NULL);
        s->thread_running = false;
    }
}

void llm_server_start(LlmServer *s)
{
    join_thread(s);
    atomic_store(&s->stopping, false);
    if (pthread_create(&s->thread, NULL, start_thread, s) == 0) s->thread_running = true;
    else set_state(s, LLM_FAILED, "Cannot start supervisor thread");
}

void llm_server_tick(LlmServer *s)
{
    LlmState st = llm_server_state(s, NULL, 0);
    /* Crashes far apart are not a crash loop: a healthy minute resets the budget. */
    if (st == LLM_READY && s->restarts > 0 && s->ready_since > 0 && now_s() - s->ready_since > HEALTHY_RESET_S)
        s->restarts = 0;
    if (st == LLM_READY && !child_alive(s)) {
        set_state(s, LLM_RESTARTING, "llama-server stopped unexpectedly; restarting\xE2\x80\xA6");
        st = LLM_RESTARTING;
    }
    if (st != LLM_RESTARTING) return;
    if (s->restart_at == 0) {
        if (s->restarts >= MAX_RESTARTS) {
            set_state(s, LLM_FAILED, "llama-server keeps crashing; gave up after %d restarts (see %s)",
                      s->restarts, s->cfg.log_path);
            return;
        }
        double delay = 1 << (s->restarts < 5 ? s->restarts : 5); /* 1, 2, 4, 8, 16 s */
        s->restart_at = now_s() + delay;
    } else if (now_s() >= s->restart_at) {
        s->restart_at = 0;
        s->restarts++;
        llm_server_start(s);
    }
}

void llm_server_stop(LlmServer *s)
{
    atomic_store(&s->stopping, true);
    join_thread(s);
    pthread_mutex_lock(&s->mu);
    pid_t pid = s->pid;
    s->pid = 0;
    pthread_mutex_unlock(&s->mu);
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status;
        for (int i = 0; i < 50; i++) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                pid = 0;
                break;
            }
            usleep(100000);
        }
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
    if (s->lock_fd >= 0) { /* only the owner cleans up the shared run dir */
        unlink(s->sock);
        unlink(s->pidfile);
        unlink(s->keyfile);
    }
    release_instance_lock(s);
    set_state(s, LLM_STOPPED, "Model stopped");
}

void llm_server_destroy(LlmServer *s)
{
    if (!s) return;
    llm_server_stop(s);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

LlmState llm_server_state(LlmServer *s, char *msg, size_t msglen)
{
    pthread_mutex_lock(&s->mu);
    LlmState st = s->state;
    if (msg && msglen) snprintf(msg, msglen, "%s", s->msg);
    pthread_mutex_unlock(&s->mu);
    return st;
}

const char *llm_server_socket(LlmServer *s) { return s->sock; }

bool llm_server_set_slot_ctx(LlmServer *s, int slot_ctx)
{
    int slots = s->cfg.slots > 0 ? s->cfg.slots : 1;
    if (slot_ctx < 2048 || slot_ctx > 32768) return false; /* the model's trained context */
    if (s->cfg.ctx_total == slot_ctx * slots) return true;
    llm_server_stop(s);
    s->cfg.ctx_total = slot_ctx * slots;
    llm_server_start(s);
    return true;
}

long long llm_server_mem_need(const LlmServerConfig *cfg, int ctx_total)
{
    struct stat st;
    long long file = stat(cfg->model_path, &st) == 0 ? (long long)st.st_size : 0;
    return file + cfg->mem_fixed + (long long)cfg->mem_per_token * (ctx_total > 0 ? ctx_total : 0);
}

const LlmServerConfig *llm_server_config(LlmServer *s) { return &s->cfg; }

int llm_server_slot_ctx(LlmServer *s)
{
    int slots = s->cfg.slots > 0 ? s->cfg.slots : 1;
    return s->cfg.ctx_total > 0 ? s->cfg.ctx_total / slots : 32768;
}
const char *llm_server_key(LlmServer *s) { return s->key; }

int llm_server_pid(LlmServer *s)
{
    pthread_mutex_lock(&s->mu);
    int pid = s->pid;
    pthread_mutex_unlock(&s->mu);
    return pid;
}

int llm_server_restarts(LlmServer *s) { return s->restarts; }

void llm_server_test_attach(LlmServer *s, const char *socket_path, const char *key)
{
    snprintf(s->sock, sizeof(s->sock), "%s", socket_path);
    snprintf(s->key, sizeof(s->key), "%s", key ? key : "");
    set_state(s, LLM_READY, "test stub");
}
