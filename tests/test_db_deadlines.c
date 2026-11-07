/* Standalone fake TCP MariaDB protocol fixture; includes the real db.c and
 * links Connector/C. No SQL tables are created or modified. Only mysql_init
 * is wrapped to permit plaintext on this loopback-only fake server; production
 * TLS defaults remain unchanged. See final build recipe.
 */
#define _DARWIN_C_SOURCE
#include "app/db.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mysql.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static MYSQL *fixture_mysql_init(MYSQL *m)
{
    m = mysql_init(m);
    if (m) {
        my_bool verify = 0;
        mysql_options(m, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
    }
    return m;
}

#define mysql_init fixture_mysql_init
#include "../src/app/db.c"
#undef mysql_init

enum { NO_GREETING, DRIP_GREETING, NO_AUTH, NO_AUTOCOMMIT, NO_SESSION, READY, SHARED_BUDGET, DROP_AUTH };
typedef struct {
    int listener, mode;
    unsigned port;
    pthread_t thread;
    atomic_bool stop, cancel;
    atomic_int accepted, stage;
    bool cancel_wait;
} Fixture;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

static void pause_ms(long n)
{
    struct timespec t = { n / 1000, n % 1000 * 1000000 };
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

static bool transfer(Fixture *f, int fd, void *data, size_t len, bool writing)
{
    char *p = data;
    while (len && !atomic_load(&f->stop)) {
        struct pollfd wait = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
        if (poll(&wait, 1, 10) <= 0) continue;
        ssize_t n = writing ? send(fd, p, len, 0) : recv(fd, p, len, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return len == 0;
}

static bool packet(Fixture *f, int fd, unsigned seq, const unsigned char *body, size_t len)
{
    unsigned char header[] = { len & 255, (len >> 8) & 255, (len >> 16) & 255, seq };
    return transfer(f, fd, header, sizeof(header), true) && transfer(f, fd, (void *)body, len, true);
}

static bool read_packet(Fixture *f, int fd)
{
    unsigned char header[4], body[4096];
    if (!transfer(f, fd, header, sizeof(header), false)) return false;
    size_t n = header[0] | (size_t)header[1] << 8 | (size_t)header[2] << 16;
    if (n > sizeof(body)) return false;
    return transfer(f, fd, body, n, false);
}

static bool greeting(Fixture *f, int fd)
{
    unsigned char b[160] = {0};
    size_t n = 0;
    b[n++] = 10;
    const char version[] = "5.5.5-10.11.0-MariaDB";
    memcpy(b + n, version, sizeof(version)); n += sizeof(version);
    b[n++] = 1; n += 3; /* connection id */
    memcpy(b + n, "12345678", 8); n += 9;
    unsigned flags = 1 | 4 | 8 | 512 | 8192 | 32768 | 131072 | 524288;
    b[n++] = flags & 255; b[n++] = (flags >> 8) & 255;
    b[n++] = 45; /* utf8mb4_general_ci */
    b[n++] = 2; b[n++] = 0; /* SERVER_STATUS_AUTOCOMMIT */
    b[n++] = (flags >> 16) & 255; b[n++] = (flags >> 24) & 255;
    b[n++] = 21; n += 10;
    memcpy(b + n, "abcdefghijkl", 12); n += 13;
    const char plugin[] = "mysql_native_password";
    memcpy(b + n, plugin, sizeof(plugin)); n += sizeof(plugin);
    if (f->mode != DRIP_GREETING) return packet(f, fd, 0, b, n);
    unsigned char header[] = { n & 255, 0, 0, 0 };
    if (!transfer(f, fd, header, sizeof(header), true)) return false;
    for (size_t i = 0; i < n; i++) {
        pause_ms(10);
        if (!transfer(f, fd, b + i, 1, true)) return false;
    }
    return true;
}

static void stall(Fixture *f)
{
    if (f->cancel_wait) {
        pause_ms(20);
        atomic_store(&f->cancel, true);
    }
    while (!atomic_load(&f->stop)) pause_ms(1);
}

static void *serve(void *arg)
{
    Fixture *f = arg;
    struct pollfd p = { .fd = f->listener, .events = POLLIN };
    while (!atomic_load(&f->stop)) {
        if (poll(&p, 1, 10) <= 0) continue;
        int fd = accept(f->listener, NULL, NULL);
        if (fd < 0) continue;
        atomic_fetch_add(&f->accepted, 1);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        fcntl(fd, F_SETFL, O_NONBLOCK);
        atomic_store(&f->stage, 1);
        if (f->mode == NO_GREETING) { stall(f); goto done; }
        if (f->mode == SHARED_BUDGET) pause_ms(25);
        if (!greeting(f, fd) || !read_packet(f, fd)) goto done;
        atomic_store(&f->stage, 2);
        if (f->mode == DROP_AUTH) goto done;
        if (f->mode == NO_AUTH) { stall(f); goto done; }
        if (f->mode == SHARED_BUDGET) pause_ms(25);
        const unsigned char ok[] = { 0, 0, 0, 2, 0, 0, 0 };
        if (!packet(f, fd, 2, ok, sizeof(ok)) || !read_packet(f, fd)) goto done;
        atomic_store(&f->stage, 3);
        if (f->mode == NO_AUTOCOMMIT) { stall(f); goto done; }
        if (f->mode == SHARED_BUDGET) pause_ms(25);
        if (!packet(f, fd, 1, ok, sizeof(ok)) || !read_packet(f, fd)) goto done;
        atomic_store(&f->stage, 4);
        if (f->mode == NO_SESSION) { stall(f); goto done; }
        if (f->mode == SHARED_BUDGET) pause_ms(25);
        if (!packet(f, fd, 1, ok, sizeof(ok))) goto done;
        atomic_store(&f->stage, 5);
        /* Wait for QUIT or cancellation, never an unbounded fixture read. */
        (void)read_packet(f, fd);
done:
        close(fd);
        break;
    }
    return NULL;
}

static void start(Fixture *f, int mode, bool cancel_wait)
{
    memset(f, 0, sizeof(*f));
    atomic_init(&f->stop, false);
    atomic_init(&f->cancel, false);
    atomic_init(&f->accepted, 0);
    atomic_init(&f->stage, 0);
    f->mode = mode;
    f->cancel_wait = cancel_wait;
    f->listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(f->listener >= 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(bind(f->listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(getsockname(f->listener, (struct sockaddr *)&addr, &len) == 0);
    f->port = ntohs(addr.sin_port);
    CHECK(listen(f->listener, 4) == 0);
    CHECK(pthread_create(&f->thread, NULL, serve, f) == 0);
}

static void stop(Fixture *f)
{
    atomic_store(&f->stop, true);
    CHECK(pthread_join(f->thread, NULL) == 0);
    close(f->listener);
}

static DbConfig config(Fixture *f)
{
    DbConfig c = { .port = f->port, .timeout_s = 10 };
    strcpy(c.host, "127.0.0.1");
    strcpy(c.user, "fixture");
    strcpy(c.password, "fixture");
    strcpy(c.database, "fixture");
    return c;
}

static void invalid_deadlines(void)
{
    Fixture f;
    start(&f, NO_GREETING, false);
    DbConfig c = config(&f);
    double deadlines[] = { NAN, INFINITY, -INFINITY, 0, -1, now_ms() - 1 };
    char err[256];
    for (size_t i = 0; i < sizeof(deadlines) / sizeof(*deadlines); i++) {
        CHECK(db_open_until(&c, NULL, deadlines[i], err, sizeof(err)) == NULL);
        CHECK(err[0] != 0);
    }
    atomic_store(&f.cancel, true);
    CHECK(db_open_until(&c, &f.cancel, now_ms() + 2000, err, sizeof(err)) == NULL);
    CHECK(strcmp(err, "cancelled") == 0);
    strcpy(c.host, "blocking-dns.invalid");
    CHECK(db_open_until(&c, NULL, now_ms() + 2000, err, sizeof(err)) == NULL);
    CHECK(strstr(err, "numeric host") != NULL);
    struct pollfd p = { .fd = f.listener, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(atomic_load(&f.accepted) == 0);
    stop(&f);
}

static void stalled_stages(void)
{
    int modes[] = { NO_GREETING, DRIP_GREETING, NO_AUTH, NO_AUTOCOMMIT, NO_SESSION, SHARED_BUDGET, DROP_AUTH };
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); i++) {
        Fixture f;
        start(&f, modes[i], false);
        DbConfig c = config(&f);
        char err[256];
        double begin = now_ms();
        Db *db = db_open_until(&c, NULL, begin + (modes[i] == SHARED_BUDGET ? 80 : 100), err, sizeof(err));
        CHECK(db == NULL);
        CHECK(db_socket(db) == -1);
        CHECK(err[0] != 0);
        if (modes[i] != DROP_AUTH) CHECK(strstr(err, "timed out") != NULL);
        CHECK(now_ms() - begin < 5000); /* watchdog, not an SLA assertion */
        if (db) db_close(db);
        stop(&f);
    }
}

static void cancelled_stages(void)
{
    int modes[] = { NO_GREETING, NO_AUTH, NO_AUTOCOMMIT, NO_SESSION };
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); i++) {
        Fixture f;
        start(&f, modes[i], true);
        DbConfig c = config(&f);
        char err[256];
        Db *db = db_open_until(&c, &f.cancel, now_ms() + 5000, err, sizeof(err));
        CHECK(db == NULL);
        CHECK(strcmp(err, "cancelled") == 0);
        CHECK(atomic_load(&f.stage) == (modes[i] == NO_GREETING ? 1 : modes[i] == NO_AUTH ? 2 : modes[i] == NO_AUTOCOMMIT ? 3 : 4));
        if (db) db_close(db);
        stop(&f);
    }
}

static void successful_setup(void)
{
    Fixture f;
    start(&f, READY, false);
    DbConfig c = config(&f);
    char err[256];
    double begin = now_ms();
    Db *db = db_open_until(&c, NULL, begin + 2000, err, sizeof(err));
    if (!db) fprintf(stderr, "fixture setup failed: %s (stage %d)\n", err, atomic_load(&f.stage));
    CHECK(db != NULL);
    CHECK(db_socket(db) >= 0);
    if (db) {
        CHECK(!db_broken(db));
        CHECK(!db_in_transaction(db));
        db_close(db);
    }
    stop(&f);
}

static int fd_count(void)
{
    int n = 0;
    for (int i = 0; i < 1024; i++) if (fcntl(i, F_GETFD) >= 0) n++;
    return n;
}

int main(void)
{
    alarm(30);
    /* Warm up Connector/C's process initialization before the FD baseline. */
    Fixture f = { .port = 1 };
    DbConfig c = config(&f);
    char err[256];
    Db *db = db_open_until(&c, NULL, now_ms() + 100, err, sizeof(err));
    if (db) db_close(db);
    int before = fd_count();
    TestCase cases[] = {
        { "invalid/expired/cancelled deadlines do not connect", invalid_deadlines },
        { "connect/auth/session stages share a deadline", stalled_stages },
        { "each suspended stage observes cancellation", cancelled_stages },
        { "complete local handshake and session setup", successful_setup },
    };
    (void)run_tests(cases, sizeof(cases) / sizeof(*cases));
    CHECK(fd_count() == before);
    return g_failures ? 1 : 0;
}
