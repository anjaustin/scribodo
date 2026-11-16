#include "app/startup_env.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool g_sealed;

void startup_env_seal(void) { atomic_store(&g_sealed, true); }

bool startup_env_sealed(void) { return atomic_load(&g_sealed); }

/* A write after the seal is a programming error: it fails loudly (assert) wherever assertions are on, which is every
 * build this tree makes; with NDEBUG it is refused and reported. */
static bool refused(const char *what, const char *name)
{
    if (!startup_env_sealed()) return false;
    fprintf(stderr, "scribodo: refused %s(%s) after the environment was sealed (threads may be reading it)\n", what, name);
    assert(!"environment written after the first thread could start");
    return true;
}

bool startup_env_set(const char *name, const char *value)
{
    if (refused("setenv", name)) return false;
    return setenv(name, value, 1) == 0;
}

bool startup_env_unset(const char *name)
{
    if (refused("unsetenv", name)) return false;
    return unsetenv(name) == 0;
}

static void copy_var(const char *name, bool *has, char *out, size_t outlen, const char *fallback)
{
    const char *v = getenv(name);
    if (has) *has = v != NULL;
    snprintf(out, outlen, "%s", v ? v : fallback);
}

/* A UI test's own copy of a file the author keeps, unless the run names one. */
static bool redirect(const StartupEnv *env, const char *name, const char *file)
{
    if (!env->scripted || getenv(name)) return true;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", env->script_dir, file);
    return startup_env_set(name, path);
}

bool startup_env_prepare(StartupEnv *env)
{
    memset(env, 0, sizeof(*env));
    copy_var("SCRIBODO_SCRIPT", &env->scripted, env->script_path, sizeof(env->script_path), "");
    copy_var("SCRIBODO_SCRIPT_DIR", &env->has_script_dir, env->script_dir, sizeof(env->script_dir), ".");
    copy_var("SCRIBODO_SHOT_DIR", &env->has_shot_dir, env->shot_dir, sizeof(env->shot_dir), "");
    assert(!startup_env_sealed() && "startup_env_prepare runs once, before any thread");
    if (startup_env_sealed()) return false;
    bool ok = redirect(env, "SCRIBODO_MODEL_SETTINGS", "model-settings.json");
    ok = startup_env_unset("SSLKEYLOGFILE") && ok;
    ok = redirect(env, "SCRIBODO_REMOTE_CONFIG", "remote.json") && ok;
    ok = redirect(env, "SCRIBODO_PROFILE", "profile.json") && ok;
    startup_env_seal();
    return ok;
}
