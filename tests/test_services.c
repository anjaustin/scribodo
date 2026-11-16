/* The local services layer seen from the app: the exit-code contract of tools/services.sh as the supervisor reads it
 * (fake scripts for every code, then the real script refusing on fake installations: nothing is started, every store
 * is a throwaway folder). Run from the repository root (make test). */
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/service_supervisor.h"
#include "app/tools_dir.h"
#include "app/backup.h"
#include "core/child_process.h"
#include <fcntl.h>
#include <sys/wait.h>
#include "test_state_fixture.h"
#include "test_util.h"

static char g_dir[PATH_MAX];
static char g_repo_tools[PATH_MAX];
static StoreManager *g_store;
static int g_case;
static char g_self[PATH_MAX];

static void write_file(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fputs(text, f);
    fclose(f);
    chmod(path, mode);
}

/* A fresh folder under the fixture for one case. */
static void fresh_dir(char *out, size_t outlen, const char *name)
{
    snprintf(out, outlen, "%s/%s-%d", g_dir, name, ++g_case);
    CHECK(mkdir(out, 0700) == 0);
}

static ServiceSupervisorResult run_up(const char *tools_dir)
{
    ServiceSupervisorResult result;
    memset(&result, 0, sizeof(result));
    ServiceSupervisor supervisor;
    CHECK(service_supervisor_init(&supervisor, g_store));
    CHECK(service_supervisor_start(&supervisor, tools_dir, true));
    bool delivered = false;
    for (int i = 0; !delivered && i < 3000; i++) { /* a minute at most */
        delivered = service_supervisor_poll(&supervisor, &result);
        if (!delivered) usleep(20000);
    }
    CHECK(delivered);
    CHECK(service_supervisor_destroy(&supervisor));
    return result;
}

/* A stand-in services.sh: BODY then exit CODE. */
static ServiceSupervisorResult run_fake(const char *body, int code)
{
    char dir[PATH_MAX], script[PATH_MAX + 32], text[8192];
    fresh_dir(dir, sizeof(dir), "fake-tools");
    snprintf(script, sizeof(script), "%s/services.sh", dir);
    snprintf(text, sizeof(text), "#!/bin/sh\n%s\nexit %d\n", body, code);
    write_file(script, text, 0700);
    return run_up(dir);
}

static void test_exit_codes(void)
{
    static const struct {
        int code;
        const char *headline;
    } codes[] = {
        { SERVICES_EXIT_FAILED, "didn't start" }, { SERVICES_EXIT_REFUSED, "refused" },
        { SERVICES_EXIT_MARIADB, "MariaDB" },     { SERVICES_EXIT_QDRANT, "Qdrant" },
        { SERVICES_EXIT_PORT, "port is taken" },  { SERVICES_EXIT_DOCKER, "Docker isn't running" },
        { SERVICES_EXIT_BUSY, "holds the store" }, { 9, "didn't start" },
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        char body[256], line[64], text[600];
        snprintf(line, sizeof(line), "the reason for code %d", codes[i].code);
        snprintf(body, sizeof(body), "echo 'an earlier line' >&2\necho 'progress on stdout'\necho '%s' >&2\necho >&2", line);
        ServiceSupervisorResult r = run_fake(body, codes[i].code);
        CHECK(r.command_attempted && !r.command_succeeded);
        CHECK(r.exit_code == codes[i].code);
        CHECK(!strcmp(r.reason, line));
        const char *headline = service_supervisor_headline(&r);
        CHECK(headline && strstr(headline, codes[i].headline));
        service_supervisor_describe(&r, text, sizeof(text));
        CHECK(headline && !strncmp(text, headline, strlen(headline)) && strstr(text, line));
    }

    ServiceSupervisorResult ok = run_fake("echo 'mode\tnative'", 0);
    char text[300] = "x";
    service_supervisor_describe(&ok, text, sizeof(text));
    CHECK(ok.command_succeeded && ok.exit_code == 0 && !service_supervisor_headline(&ok) && !text[0]);

    /* A start that succeeded with a warning: shown (without its prefix), and only then. */
    ServiceSupervisorResult warned = run_fake("echo 'warning: MariaDB 11.4.14 is running; the store is pinned to 11.4.13' >&2", 0);
    CHECK(warned.command_succeeded && !service_supervisor_headline(&warned));
    CHECK(service_supervisor_warning(&warned) && !strcmp(service_supervisor_warning(&warned), "MariaDB 11.4.14 is running; the store is pinned to 11.4.13"));
    CHECK(!service_supervisor_warning(&ok));
    ServiceSupervisorResult failed_warning = run_fake("echo 'warning: not after a failure' >&2", 1);
    CHECK(!service_supervisor_warning(&failed_warning));

    /* No line on stderr: the code is still said. */
    ServiceSupervisorResult quiet = run_fake(":", SERVICES_EXIT_PORT);
    service_supervisor_describe(&quiet, text, sizeof(text));
    CHECK(quiet.exit_code == SERVICES_EXIT_PORT && !quiet.reason[0] && strstr(text, "exit 5"));
}

static void test_reason_bounds(void)
{
    /* A megabyte on stderr neither blocks the script nor grows the reason past its bound; control characters in
     * the line become spaces. */
    ServiceSupervisorResult big = run_fake("head -c 1048576 /dev/zero | tr '\\0' 'x' >&2\necho >&2\n"
                                           "printf 'tab\\there\\033[31mred\\n' >&2",
                                           1);
    CHECK(big.exit_code == 1 && !strcmp(big.reason, "tab here [31mred"));
    ServiceSupervisorResult long_line = run_fake("head -c 5000 /dev/zero | tr '\\0' 'y' >&2", 1);
    CHECK(long_line.exit_code == 1 && strlen(long_line.reason) == SERVICES_REASON_MAX - 1 && long_line.reason[0] == 'y');

    /* A script that is not there: said, not a silent false. */
    char dir[PATH_MAX];
    fresh_dir(dir, sizeof(dir), "no-script");
    ServiceSupervisorResult missing = run_up(dir);
    CHECK(missing.command_attempted && !missing.command_succeeded && missing.exit_code == -1);
    CHECK(strstr(missing.reason, "couldn't run") && strstr(missing.reason, "services.sh"));
    CHECK(strstr(service_supervisor_headline(&missing), "couldn't run"));
}

static bool valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int extra = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0) return false;
        for (int i = 1; i <= extra; i++)
            if ((p[i] & 0xC0) != 0x80) return false;
        p += 1 + extra;
    }
    return true;
}

/* A long line of multi-byte characters is cut at a character boundary, in the reason and in its description. */
static void test_reason_utf8(void)
{
    /* 200 x "é" (2 bytes): a byte cut at 239 would split the 120th. */
    ServiceSupervisorResult r = run_fake("i=0; while [ $i -lt 200 ]; do printf '\303\251' >&2; i=$((i+1)); done; echo >&2", 1);
    CHECK(r.exit_code == 1 && valid_utf8(r.reason));
    CHECK(strlen(r.reason) == SERVICES_REASON_MAX - 2 && !strncmp(r.reason, "\xC3\xA9", 2)); /* 119 whole: 238 bytes */
    char text[300];
    service_supervisor_describe(&r, text, sizeof(text));
    CHECK(valid_utf8(text) && strlen(text) < sizeof(text) && strstr(text, "didn't start: \xC3\xA9"));
    for (size_t size = 1; size < 64; size++) { /* every small buffer, so every cut position */
        char small[64];
        service_supervisor_describe(&r, small, size);
        CHECK(valid_utf8(small) && strlen(small) < size);
    }
    /* "€" (3 bytes) lines too */
    r = run_fake("i=0; while [ $i -lt 100 ]; do printf '\342\202\254' >&2; i=$((i+1)); done; echo >&2", 1);
    CHECK(valid_utf8(r.reason) && strlen(r.reason) == 237);
}

/* The real script, on a throwaway store whose MariaDB and Qdrant are stand-ins: it refuses before starting anything. */
typedef struct {
    char secrets[PATH_MAX];
    char bin[PATH_MAX];
    char qdrant[PATH_MAX + 16];
} FakeInstall;

static void fake_install(FakeInstall *f, const char *mariadb_version, bool qdrant_built)
{
    char root[PATH_MAX], path[PATH_MAX + 32], text[256];
    fresh_dir(root, sizeof(root), "store");
    snprintf(f->secrets, sizeof(f->secrets), "%s/secrets", root);
    snprintf(f->bin, sizeof(f->bin), "%s/bin", root);
    snprintf(f->qdrant, sizeof(f->qdrant), "%s/qdrant", root);
    CHECK(mkdir(f->bin, 0700) == 0);
    if (mariadb_version) {
        snprintf(path, sizeof(path), "%s/mariadbd", f->bin);
        snprintf(text, sizeof(text), "#!/bin/sh\necho \"$0  Ver %s-MariaDB for osx10.21 on arm64 (Homebrew)\"\n",
                 mariadb_version);
        write_file(path, text, 0700);
    }
    if (qdrant_built) write_file(f->qdrant, "#!/bin/sh\nexit 1\n", 0700);
    char logs[PATH_MAX + 8];
    snprintf(logs, sizeof(logs), "%s/logs", root);
    setenv("SCRIBODO_SECRETS", f->secrets, 1);
    setenv("SCRIBODO_LOGS", logs, 1);
    setenv("SCRIBODO_MARIADB_BIN", f->bin, 1);
    setenv("SCRIBODO_QDRANT_BIN", f->qdrant, 1);
    setenv("SCRIBODO_SERVICES", "native", 1);
    setenv("SCRIBODO_COMPOSE_PROJECT", "scribodo-test-services", 1);
    unsetenv("SCRIBODO_NATIVE_FRESH");
    unsetenv("SCRIBODO_SERVICES_LOCKED");
}

static bool exists(const char *dir, const char *name)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    return stat(path, &st) == 0;
}

static void test_real_script_refusals(void)
{
    FakeInstall f;

    fake_install(&f, NULL, true);
    ServiceSupervisorResult r = run_up(g_repo_tools);
    CHECK(r.exit_code == SERVICES_EXIT_MARIADB && strstr(r.reason, "MariaDB") && strstr(r.reason, "isn't installed"));
    CHECK(!exists(f.secrets, "db_root_password") && !exists(f.secrets, "mariadb"));

    fake_install(&f, "10.11.9", true);
    r = run_up(g_repo_tools);
    CHECK(r.exit_code == SERVICES_EXIT_MARIADB && strstr(r.reason, "MariaDB at ") && strstr(r.reason, "is 10.11.9; the store needs 11.4.13"));

    /* An older patch of the series: refused by name (AUDIT-004 F13). A newer one passes the check (the next step,
     * here the missing Qdrant, is what refuses), and an override moves the floor. */
    fake_install(&f, "11.4.12", true);
    r = run_up(g_repo_tools);
    CHECK(r.exit_code == SERVICES_EXIT_MARIADB && strstr(r.reason, "is 11.4.12; the store needs 11.4.13"));
    fake_install(&f, "11.4.14", false);
    r = run_up(g_repo_tools);
    CHECK(r.exit_code == SERVICES_EXIT_QDRANT);
    fake_install(&f, "11.4.14", true);
    setenv("SCRIBODO_MARIADB_VERSION", "11.4.15", 1);
    r = run_up(g_repo_tools);
    unsetenv("SCRIBODO_MARIADB_VERSION");
    CHECK(r.exit_code == SERVICES_EXIT_MARIADB && strstr(r.reason, "the store needs 11.4.15"));

    fake_install(&f, "11.4.13", false);
    r = run_up(g_repo_tools);
    CHECK(r.exit_code == SERVICES_EXIT_QDRANT && strstr(r.reason, "make qdrant"));

    /* Both missing: the code and the last line agree (MariaDB). */
    fake_install(&f, NULL, false);
    r = run_up(g_repo_tools);
    CHECK(r.exit_code == SERVICES_EXIT_MARIADB && strstr(r.reason, "MariaDB"));

    /* Credentials whose store is still in the containers. */
    fake_install(&f, "11.4.13", true);
    CHECK(mkdir(f.secrets, 0700) == 0);
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/db_root_password", f.secrets);
    write_file(path, "0123456789abcdef\n", 0600);
    r = run_up(g_repo_tools);
    CHECK(r.exit_code == SERVICES_EXIT_REFUSED && strstr(r.reason, "migrate-from-docker"));
    CHECK(!exists(f.secrets, "mariadb"));

    /* A port taken by another program: an initialised native store (no daemon), MariaDB's port held here. */
    fake_install(&f, "11.4.13", true);
    char data[PATH_MAX + 64];
    snprintf(data, sizeof(data), "%s/mariadb/data/mysql", f.secrets);
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && touch '%s/mariadb/initialized'", data, f.secrets);
    CHECK(system(cmd) == 0);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t alen = sizeof(a);
    CHECK(s >= 0 && bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 1) == 0 &&
          getsockname(s, (struct sockaddr *)&a, &alen) == 0);
    char port[16];
    snprintf(port, sizeof(port), "%u", (unsigned)ntohs(a.sin_port));
    setenv("SCRIBODO_DB_PORT", port, 1);
    r = run_up(g_repo_tools);
    CHECK(r.exit_code == SERVICES_EXIT_PORT && strstr(r.reason, port) && strstr(r.reason, "is taken"));
    if (s >= 0) close(s);
    unsetenv("SCRIBODO_DB_PORT");
}

/* Where the scripts are found: the override, a bundle's Resources/tools, the source tree's tools/. */
static void test_tools_dir(void)
{
    char root[PATH_MAX], cmd[PATH_MAX * 2], out[PATH_MAX + 64], want[PATH_MAX + 64];
    fresh_dir(root, sizeof(root), "layout");
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && mkdir -p Scribodo.app/Contents/MacOS Scribodo.app/Contents/Resources/tools tree/build tree/tools "
             "empty/build && touch Scribodo.app/Contents/Resources/tools/services.sh tree/tools/services.sh && "
             "chmod 755 Scribodo.app/Contents/Resources/tools/services.sh tree/tools/services.sh",
             root);
    CHECK(system(cmd) == 0);
    unsetenv("SCRIBODO_TOOLS_DIR");
    char app[PATH_MAX + 64];

    snprintf(app, sizeof(app), "%s/Scribodo.app/Contents/MacOS/", root);
    CHECK(tools_dir_resolve(app, out, sizeof(out)));
    snprintf(want, sizeof(want), "%s../Resources/tools", app);
    CHECK(!strcmp(out, want));

    snprintf(app, sizeof(app), "%s/tree/build/", root);
    CHECK(tools_dir_resolve(app, out, sizeof(out)));
    snprintf(want, sizeof(want), "%s../tools", app);
    CHECK(!strcmp(out, want));

    snprintf(app, sizeof(app), "%s/tree/build", root); /* no trailing slash */
    CHECK(tools_dir_resolve(app, out, sizeof(out)));
    snprintf(want, sizeof(want), "%s/../tools", app);
    CHECK(!strcmp(out, want));

    /* Nothing there: false, and nothing in `out`. */
    snprintf(app, sizeof(app), "%s/empty/build/", root);
    out[0] = 'x';
    CHECK(!tools_dir_resolve(app, out, sizeof(out)) && !out[0]);

    /* The override wins, as given, even over a bundle. */
    snprintf(want, sizeof(want), "%s/tree/tools", root);
    setenv("SCRIBODO_TOOLS_DIR", want, 1);
    snprintf(app, sizeof(app), "%s/Scribodo.app/Contents/MacOS/", root);
    CHECK(tools_dir_resolve(app, out, sizeof(out)) && !strcmp(out, want));
    setenv("SCRIBODO_TOOLS_DIR", root, 1);
    CHECK(!tools_dir_resolve(app, out, sizeof(out)) && !out[0]);
    setenv("SCRIBODO_TOOLS_DIR", "", 1); /* empty: not set */
    CHECK(tools_dir_resolve(app, out, sizeof(out)) && strstr(out, "Resources/tools"));
    unsetenv("SCRIBODO_TOOLS_DIR");

    /* A buffer too small for the path: false and empty, never a truncated path that names another folder. */
    char tiny[8] = "x";
    CHECK(!tools_dir_resolve(app, tiny, sizeof(tiny)) && !tiny[0]);
    char shorter[PATH_MAX];
    snprintf(app, sizeof(app), "%s/tree/build/", root);
    size_t need = strlen(app) + strlen("../tools") + 1;
    CHECK(need < sizeof(shorter) && !tools_dir_resolve(app, shorter, need - 1) && !shorter[0]);
    CHECK(tools_dir_resolve(app, shorter, need));
}

/* --list-fds: this binary as the child. It prints which of 0-2 are open and how many of its other descriptors are one
 * of the parent's held files ("fds 0 1 2 leaks 0"). Held files are named by device and inode in SCRIBODO_TEST_HELD,
 * so a descriptor a sanitizer runtime opens for itself at start (seen on hosted CI under ASan) is not counted, and a
 * leaked descriptor is counted whatever number it has. */
static int list_fds_main(void)
{
    char line[256] = "fds";
    size_t n = strlen(line);
    for (int fd = 0; fd < 3; fd++)
        if (fcntl(fd, F_GETFD) != -1) n += (size_t)snprintf(line + n, sizeof(line) - n, " %d", fd);
    const char *held = getenv("SCRIBODO_TEST_HELD");
    int leaks = 0;
    for (int fd = 3; fd < 4096; fd++) {
        struct stat st;
        if (fcntl(fd, F_GETFD) == -1 || fstat(fd, &st) != 0) continue;
        char id[64];
        snprintf(id, sizeof(id), ",%llu:%llu,", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
        if (!held || strstr(held, id)) leaks++; /* no list: every extra descriptor counts */
    }
    snprintf(line + n, sizeof(line) - n, " leaks %d", leaks);
    fprintf(stdout, "%s\n", line);
    fprintf(stderr, "%s\n", line);
    return 1;
}

/* Descriptors the app holds without close-on-exec, as the register's flock descriptor was: none may reach a child. */
static void hold_leaky_descriptors(int held[3])
{
    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/held", g_dir);
    held[0] = open(path, O_WRONLY | O_CREAT, 0600);
    int p[2] = { -1, -1 };
    CHECK(pipe(p) == 0);
    held[1] = p[0];
    held[2] = p[1];
    for (int i = 0; i < 3; i++) CHECK(held[i] > 2 && !(fcntl(held[i], F_GETFD) & FD_CLOEXEC));
    char ids[256] = ",";
    for (int i = 0; i < 3; i++) {
        struct stat st;
        if (held[i] >= 0 && fstat(held[i], &st) == 0)
            snprintf(ids + strlen(ids), sizeof(ids) - strlen(ids), "%llu:%llu,", (unsigned long long)st.st_dev,
                     (unsigned long long)st.st_ino);
    }
    setenv("SCRIBODO_TEST_HELD", ids, 1); /* read by --list-fds in the children */
}

static void release(int held[3])
{
    for (int i = 0; i < 3; i++)
        if (held[i] >= 0) close(held[i]);
    unsetenv("SCRIBODO_TEST_HELD");
}

/* Each spawn site hands its child 0-2 and nothing else (AUDIT-004; the register's "spawned tools inherit the app's
 * descriptors"): the shared helper, the services supervisor, the backup pipeline; and the server's pid check
 * spawns nothing. */
static void test_child_descriptors(void)
{
    int held[3];
    hold_leaky_descriptors(held);

    /* The helper: stdout on a pipe, stderr to /dev/null. */
    int out[2];
    CHECK(child_pipe(out) && out[0] > 2 && out[1] > 2 && (fcntl(out[0], F_GETFD) & FD_CLOEXEC));
    char *argv[] = { g_self, "--list-fds", NULL };
    pid_t pid = -1;
    CHECK(child_spawn(&pid, g_self, argv, out[1], -1, true) == 0);
    close(out[1]);
    char buf[512] = "";
    ssize_t n, got = 0;
    while ((n = read(out[0], buf + got, sizeof(buf) - 1 - (size_t)got)) > 0) got += n;
    buf[got] = '\0';
    close(out[0]);
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st));
    CHECK(!strcmp(buf, "fds 0 1 2 leaks 0\n"));

    /* The supervisor: the script is the child, and execs the lister with its stderr (the supervisor's pipe). */
    char dir[PATH_MAX], script[PATH_MAX + 32], text[PATH_MAX + 64];
    fresh_dir(dir, sizeof(dir), "fds-tools");
    snprintf(script, sizeof(script), "%s/services.sh", dir);
    snprintf(text, sizeof(text), "#!/bin/sh\nexec '%s' --list-fds\n", g_self);
    write_file(script, text, 0700);
    ServiceSupervisorResult r = run_up(dir);
    CHECK(r.exit_code == 1 && !strcmp(r.reason, "fds 0 1 2 leaks 0"));

    /* The backup pipeline: backup.sh is the child, the lister's stderr is the error line. */
    snprintf(script, sizeof(script), "%s/backup.sh", dir);
    write_file(script, text, 0700);
    char project[PATH_MAX + 16], backups[PATH_MAX + 16], dest[PATH_MAX + 64] = "", why[400] = "";
    snprintf(project, sizeof(project), "%s/project", dir);
    snprintf(backups, sizeof(backups), "%s/backups", dir);
    CHECK(mkdir(project, 0700) == 0);
    setenv("SCRIBODO_BACKUPS", backups, 1);
    CHECK(!backup_project(g_store, dir, project, dest, sizeof(dest), why, sizeof(why)));
    CHECK(strstr(why, ": fds 0 1 2 leaks 0") && why[strlen(why) - 1] == '0');
    unsetenv("SCRIBODO_BACKUPS");
    release(held);

    /* The server's pid check reads arguments from the kernel: a known child's, a dead one's. */
    char *sleeper[] = { "/bin/sleep", "30", NULL };
    CHECK(child_spawn(&pid, "/bin/sleep", sleeper, -1, -1, true) == 0);
    char args[256] = "";
    bool read_ok = false;
    for (int i = 0; i < 200 && !(read_ok = child_process_args(pid, args, sizeof(args)) && !strcmp(args, "/bin/sleep 30")); i++)
        usleep(5000); /* until the exec has happened */
    CHECK(read_ok);
    CHECK(getpgid(pid) == pid); /* own_group */
    char tiny[6];
    CHECK(child_process_args(pid, tiny, sizeof(tiny)) && !strcmp(tiny, "/bin/"));
    kill(pid, SIGKILL);
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(!child_process_args(pid, args, sizeof(args)));
}

/* The script's header and the C enum name the same codes. */
static void test_contract_written_once(void)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/services.sh", g_repo_tools);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    char line[512];
    int seen[128] = { 0 }, count = 0;
    bool in_block = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "# Exit codes:", 13)) in_block = true;
        else if (in_block && strncmp(line, "#", 1)) break;
        int code;
        char rest[8];
        if (in_block && sscanf(line, "#   %d %7s", &code, rest) == 2 && code >= 0 && code < 128) {
            seen[code]++;
            count++;
        }
    }
    fclose(f);
    const int expected[] = { SERVICES_EXIT_OK,     SERVICES_EXIT_FAILED, SERVICES_EXIT_REFUSED, SERVICES_EXIT_MARIADB,
                             SERVICES_EXIT_QDRANT, SERVICES_EXIT_PORT,   SERVICES_EXIT_DOCKER,  SERVICES_EXIT_BUSY };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) CHECK(seen[expected[i]] == 1);
    CHECK(count == (int)(sizeof(expected) / sizeof(expected[0])));
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--list-fds")) return list_fds_main();
    if (!realpath(argv[0], g_self)) return 1;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return 1;
    snprintf(g_repo_tools, sizeof(g_repo_tools), "%s/tools", cwd);
    char probe[PATH_MAX + 32];
    snprintf(probe, sizeof(probe), "%s/services.sh", g_repo_tools);
    if (access(probe, X_OK) != 0) {
        fprintf(stderr, "test_services: run from the repository root (no %s)\n", probe);
        return 1;
    }
    if (!test_state_fixture_init(g_dir, sizeof(g_dir), &g_store)) return 1;
    /* The probe after each start reads this folder's (absent) password: never the author's store. */
    setenv("SCRIBODO_SECRETS", g_dir, 1);
    TestCase tests[] = {
        { "exit_codes", test_exit_codes },
        { "reason_bounds", test_reason_bounds },
        { "reason_utf8", test_reason_utf8 },
        { "real_script_refusals", test_real_script_refusals },
        { "contract_written_once", test_contract_written_once },
        { "tools_dir", test_tools_dir },
        { "child_descriptors", test_child_descriptors },
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    test_state_fixture_destroy(g_dir, g_store);
    return rc;
}
