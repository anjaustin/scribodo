/* The process environment is written only before the first thread starts (AUDIT-004 F12: main() set
 * SCRIBODO_REMOTE_CONFIG and SCRIBODO_PROFILE after the model supervisors, memory job and observer had started, and
 * getenv/setenv across threads is unsynchronised). startup_env_prepare runs first in main(), reads the UI-test
 * variables once, applies the UI-test redirects, and seals the environment; every later write is refused and
 * reported. All writes to the environment in src/ go through here (tests/test_env_writes.py). */
#ifndef SCRIBODO_STARTUP_ENV_H
#define SCRIBODO_STARTUP_ENV_H

#include <limits.h>
#include <stdbool.h>

typedef struct {
    bool scripted;               /* SCRIBODO_SCRIPT is set: a UI test run */
    char script_path[PATH_MAX];  /* SCRIBODO_SCRIPT */
    bool has_script_dir;         /* SCRIBODO_SCRIPT_DIR is set */
    char script_dir[PATH_MAX];   /* SCRIBODO_SCRIPT_DIR, or "." when unset */
    bool has_shot_dir;
    char shot_dir[PATH_MAX];     /* SCRIBODO_SHOT_DIR */
} StartupEnv;

/* Reads SCRIBODO_SCRIPT, SCRIBODO_SCRIPT_DIR and SCRIBODO_SHOT_DIR once. A UI test keeps its own model settings,
 * remote configuration and profile in its fixture folder (SCRIBODO_MODEL_SETTINGS, SCRIBODO_REMOTE_CONFIG,
 * SCRIBODO_PROFILE, each only when not already set), so the author's never reach a run's artefacts; TLS secrets are
 * never written out for a remote model's traffic (SSLKEYLOGFILE removed). Then seals the environment. False when a
 * write failed or the environment was already sealed. */
bool startup_env_prepare(StartupEnv *env);
/* setenv/unsetenv, refused (false, and reported on stderr) once sealed. */
bool startup_env_set(const char *name, const char *value);
bool startup_env_unset(const char *name);
void startup_env_seal(void);
bool startup_env_sealed(void);

#endif
