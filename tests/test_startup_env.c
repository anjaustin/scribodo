/* The environment is written only before the first thread starts (app/startup_env.h, AUDIT-004 F12). One process:
 * the cases run in order, sealing at the end of the first. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include "app/startup_env.h"
#include "test_util.h"

/* A UI test run: its fixture folder supplies the files the author keeps, unless the run names its own; the TLS key
 * log is removed; the variables are read once into the struct. */
static void test_prepare_scripted(void)
{
    setenv("SCRIBODO_SCRIPT", "tests/ui/x.scribo", 1);
    setenv("SCRIBODO_SCRIPT_DIR", "/fixture", 1);
    unsetenv("SCRIBODO_SHOT_DIR");
    unsetenv("SCRIBODO_MODEL_SETTINGS");
    setenv("SCRIBODO_REMOTE_CONFIG", "/run/own-remote.json", 1); /* named by the run: kept */
    unsetenv("SCRIBODO_PROFILE");
    setenv("SSLKEYLOGFILE", "/tmp/keys", 1);
    CHECK(!startup_env_sealed());
    StartupEnv env;
    CHECK(startup_env_prepare(&env));
    CHECK(startup_env_sealed());
    CHECK(env.scripted && !strcmp(env.script_path, "tests/ui/x.scribo"));
    CHECK(env.has_script_dir && !strcmp(env.script_dir, "/fixture") && !env.has_shot_dir);
    const char *v = getenv("SCRIBODO_MODEL_SETTINGS");
    CHECK(v && !strcmp(v, "/fixture/model-settings.json"));
    v = getenv("SCRIBODO_REMOTE_CONFIG");
    CHECK(v && !strcmp(v, "/run/own-remote.json"));
    v = getenv("SCRIBODO_PROFILE");
    CHECK(v && !strcmp(v, "/fixture/profile.json"));
    CHECK(getenv("SSLKEYLOGFILE") == NULL);
}

/* Once sealed, every write is refused and the environment is left as it was; a second prepare is refused too. */
static void test_sealed_refuses_writes(void)
{
    CHECK(startup_env_sealed());
    CHECK(!startup_env_set("SCRIBODO_PROFILE", "/elsewhere.json"));
    const char *v = getenv("SCRIBODO_PROFILE");
    CHECK(v && !strcmp(v, "/fixture/profile.json"));
    CHECK(!startup_env_unset("SCRIBODO_PROFILE") && getenv("SCRIBODO_PROFILE") != NULL);
    StartupEnv env;
    CHECK(!startup_env_prepare(&env));
    CHECK(env.scripted && !strcmp(env.script_dir, "/fixture")); /* still read */
}

int main(void)
{
    TestCase tests[] = {
        { "prepare_scripted", test_prepare_scripted },
        { "sealed_refuses_writes", test_sealed_refuses_writes },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
