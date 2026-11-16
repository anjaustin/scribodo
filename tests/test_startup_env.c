/* The environment is written only before the first thread starts (app/startup_env.h, AUDIT-004 F12). One process:
 * the cases run in order, sealing at the end of the first. */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
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
    unsetenv("SCRIBODO_APPEARANCE");
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
    v = getenv("SCRIBODO_APPEARANCE");
    CHECK(v && !strcmp(v, "/fixture/appearance.json"));
    CHECK(getenv("SSLKEYLOGFILE") == NULL);
}

/* Runs `fn` in a child; true when the child died of SIGABRT (the assertion), with the environment of the parent
 * untouched either way. */
static bool aborts(void (*fn)(void))
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr); /* the expected assertion message */
        fn();
        _exit(0);
    }
    int status = 0;
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

static void set_after_seal(void) { (void)startup_env_set("SCRIBODO_PROFILE", "/elsewhere.json"); }
static void unset_after_seal(void) { (void)startup_env_unset("SCRIBODO_PROFILE"); }
static void prepare_again(void)
{
    StartupEnv env;
    (void)startup_env_prepare(&env);
}

/* Once sealed, a write fails loudly (the assertion aborts) and a second prepare does too. */
static void test_sealed_writes_fail_loudly(void)
{
    CHECK(startup_env_sealed());
    CHECK(aborts(set_after_seal));
    CHECK(aborts(unset_after_seal));
    CHECK(aborts(prepare_again));
    const char *v = getenv("SCRIBODO_PROFILE");
    CHECK(v && !strcmp(v, "/fixture/profile.json"));
}

int main(void)
{
    TestCase tests[] = {
        { "prepare_scripted", test_prepare_scripted },
        { "sealed_writes_fail_loudly", test_sealed_writes_fail_loudly },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
