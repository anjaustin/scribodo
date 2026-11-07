#ifndef SCRIBODO_GENERATIVE_GATE_H
#define SCRIBODO_GENERATIVE_GATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct GenerativeGate GenerativeGate;
typedef struct GenerativeTicket GenerativeTicket;

/* Zero-initialize each ticket. Tickets are caller-owned, address-stable and
 * non-copyable while pending/admitted. Only their owner calls ticket APIs.
 * cancel is caller-owned, initialized before registration, and must outlive
 * finish(); the gate only stores it while pending/admitted, under its mutex.
 * Never clear cancel while a ticket is registered. No callbacks run here.
 *
 * Foreground: begin ONCE per request, poll try_foreground on a worker with a
 * bounded deadline, finish on EVERY exit (including cancellation/timeouts).
 * Background: try_background, perform generation, finish. Failed background
 * admission leaves the ticket unregistered. Neither path waits for a permit
 * or performs I/O; mutex sections only update admission bookkeeping.
 * The permit must cover the entire HTTP call, including cancellation drain.
 * HTTP cancellation cannot guarantee the server has stopped computing: the
 * server must honor disconnected/cancelled requests for physical exclusivity.
 *
 * close blocks new admissions and cancels borrowers. Join all borrowers and
 * detach idle users before destroy. destroy returns false without freeing if
 * tickets remain; it cannot detect an idle external borrowed gate pointer or
 * race safely with new API callers. Gate lifetime is the application's duty.
 */
struct GenerativeTicket {
    GenerativeGate *gate;
    uint64_t serial, fence;
    bool foreground, admitted;
    atomic_bool *cancel;
    GenerativeTicket *next;
};

GenerativeGate *generative_gate_create(void);
void generative_gate_close(GenerativeGate *gate);
bool generative_gate_destroy(GenerativeGate *gate);
bool generative_gate_foreground_begin(GenerativeGate *gate, GenerativeTicket *ticket, atomic_bool *cancel);
bool generative_gate_try_foreground(GenerativeTicket *ticket);
bool generative_gate_try_background(GenerativeGate *gate, GenerativeTicket *ticket, atomic_bool *cancel);
/* Cancels queued intent or releases admission; stale/copied tickets fail. */
bool generative_gate_finish(GenerativeTicket *ticket);
/* Snapshot before a background operation; recheck before publishing even
 * after release. Every foreground intent and close invalidates old fences.
 * A fence check is a publication boundary, not a lock around database I/O. */
uint64_t generative_gate_fence(GenerativeGate *gate);
bool generative_gate_fence_valid(GenerativeGate *gate, uint64_t fence);
bool generative_gate_ticket_valid(GenerativeTicket *ticket);

#endif
