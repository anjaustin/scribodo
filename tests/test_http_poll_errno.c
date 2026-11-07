/* Deterministically model a successful clock probe changing errno between poll
 * and its error check. POSIX does not require successful calls to preserve it. */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <poll.h>
#include <time.h>
#include "test_util.h"

static int polls, poll_failure;
static int clobber_clock(clockid_t clock, struct timespec *out)
{
    int rc = clock_gettime(clock, out);
    if (!rc) errno = EDOM;
    return rc;
}
static int controlled_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)fds; (void)count; (void)timeout;
    polls++;
    if (polls == 1) { errno = poll_failure; return -1; }
    return 1;
}
#define clock_gettime clobber_clock
#define poll controlled_poll
#include "../src/llm/http.c"
#undef poll
#undef clock_gettime

static void preserved_errors(void)
{
    char error[160] = "";
    poll_failure = EINTR; polls = 0;
    CHECK(wait_ready(-1, POLLIN, NULL, now_ms() + 1000, error, sizeof(error)));
    CHECK(polls == 2);
    poll_failure = EBADF; polls = 0;
    CHECK(!wait_ready(-1, POLLIN, NULL, now_ms() + 1000, error, sizeof(error)));
    CHECK(polls == 1 && strstr(error, strerror(EBADF)));
}
int main(void)
{
    const TestCase tests[] = {{"poll errno survives deadline checks", preserved_errors}};
    return run_tests(tests, 1);
}
