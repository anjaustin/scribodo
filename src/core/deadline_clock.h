/* The clock every deadline, budget and timeout in Scribodo is measured on.
 *
 * On macOS CLOCK_MONOTONIC keeps counting while the system sleeps, so a
 * 500 ms memory budget or a 120 s receive timeout that straddles a laptop
 * sleep fires the moment the machine wakes (measured in MEMORY-017: a
 * request sent one second before a 942 s sleep was cancelled on wake, having
 * had one second of real time to run). CLOCK_UPTIME_RAW does not count
 * sleep: deadlines measure the time in which the work could actually make
 * progress. Local servers, the database and the UI are all paused by the same
 * sleep, so nothing they owe is late when the machine wakes. A connection a
 * remote host dropped during sleep still fails promptly through its socket.
 *
 * Every producer and consumer of an absolute deadline must use this one
 * clock; two clocks in one deadline comparison is the bug this header exists
 * to prevent. Linux has no sleep-excluding POSIX clock; the MVP targets macOS. */
#ifndef SCRIBODO_DEADLINE_CLOCK_H
#define SCRIBODO_DEADLINE_CLOCK_H

#include <time.h>

#ifdef __APPLE__
#define SCRIBODO_DEADLINE_CLOCK CLOCK_UPTIME_RAW
#else
#define SCRIBODO_DEADLINE_CLOCK CLOCK_MONOTONIC
#endif

#endif
