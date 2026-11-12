/* Deterministic process-death points for the interruption tests (GAP-004).
 *
 * A transition that must survive a crash (a migration, an atomic file
 * replacement, a store import's marker and cleanup) names the moments between
 * its steps with crash_point("area.step"). The product build compiles each
 * call to nothing: no string, no branch, no environment read. A test binary
 * built with -DSCRIBODO_CRASH_POINTS links tests/crash_point.c, where the
 * named point kills the process with SIGKILL on the hit that
 * SCRIBODO_CRASH_POINT selects ("area.step" for the first hit, "area.step#N"
 * for the Nth), and the driver (tests/test_crash_points.c) starts the next
 * process and checks what it finds. The matrix of points, invariants and
 * limits is docs/GAP-004-interruption-matrix.md. */
#ifndef SCRIBODO_CRASH_POINT_H
#define SCRIBODO_CRASH_POINT_H

#ifdef SCRIBODO_CRASH_POINTS
void crash_point(const char *name);
long crash_point_hits(void); /* hits of the selected point so far in this process (0 when none is selected) */
#else
#define crash_point(name) ((void)0)
#endif

#endif
