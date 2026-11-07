#define _POSIX_C_SOURCE 200809L
#include "llm/memory_budget.h"
#include "test_util.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct Db { int fd; bool transaction; };
int db_socket(Db *db) { return db ? db->fd : -1; }
bool db_in_transaction(Db *db) { return db && db->transaction; }

static void wait_stopped(MemoryBudget *budget)
{
    for (unsigned i = 0; i < 5000 && !atomic_load(&budget->stop); i++) {
        struct timespec delay = { .tv_nsec = 1000000 }; nanosleep(&delay, NULL);
    }
    CHECK(atomic_load(&budget->stop));
}

static void deadline_and_cancel(void)
{
    MemoryBudget budget; atomic_bool cancel; atomic_init(&cancel, false);
    CHECK(!memory_budget_begin(NULL, &cancel, 10));
    CHECK(!memory_budget_begin(&budget, &cancel, 0));
    CHECK(memory_budget_begin(&budget, &cancel, 20));
    wait_stopped(&budget); CHECK(memory_budget_expired(&budget));
    memory_budget_end(&budget); CHECK(!budget.started);
    CHECK(memory_budget_begin(&budget, &cancel, 10000));
    atomic_store(&cancel, true); wait_stopped(&budget);
    CHECK(!memory_budget_expired(&budget));
    memory_budget_end(&budget);
    CHECK(!memory_budget_begin(&budget, &cancel, 10));
}

static void owned_socket(void)
{
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Db db = { .fd = pair[0] }; MemoryBudget budget;
    atomic_bool cancel; atomic_init(&cancel, false);
    CHECK(memory_budget_begin(&budget, &cancel, 10000));
    db.transaction = true; CHECK(!memory_budget_bind_db(&budget, &db)); db.transaction = false;
    CHECK(memory_budget_bind_db(&budget, &db));
    CHECK(!memory_budget_bind_db(&budget, &db));
    /* The SQL client may close its original descriptor on an error. Reusing
     * that number must never cause our guard to shut down another socket. */
    close(pair[0]);
    int unrelated[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, unrelated) == 0);
    atomic_store(&cancel, true);
    wait_stopped(&budget);
    char byte;
    struct pollfd readable = { .fd = pair[1], .events = POLLIN };
    int ready = poll(&readable, 1, 5000);
    CHECK(ready == 1);
    if (ready == 1) CHECK(read(pair[1], &byte, 1) == 0);
    CHECK(write(unrelated[0], "x", 1) == 1 && read(unrelated[1], &byte, 1) == 1 && byte == 'x');
    memory_budget_end(&budget);
    close(pair[1]); close(unrelated[0]); close(unrelated[1]);
}

int main(void)
{
    TestCase tests[] = { { "total budget and cancellation", deadline_and_cancel }, { "owned SQL socket abort", owned_socket } };
    return run_tests(tests, sizeof(tests) / sizeof(*tests));
}
