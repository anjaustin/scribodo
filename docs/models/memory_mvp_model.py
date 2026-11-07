#!/usr/bin/env python3
"""Finite design checks, not an implementation/integration/quality test.

Run with Python 3. Models two source/policy/lease versions, two projects,
SQL availability, local/remote requests, delayed index delivery and three
out-of-order work packets. It never opens a database or calls a model/network.
"""

from collections import deque
from dataclasses import dataclass, replace
from itertools import permutations, product


@dataclass(frozen=True)
class State:
    source: int = 1
    policy: int = 1
    allowed: bool = True
    database: bool = True
    remote: bool = False
    point: int = 0
    point_project: int = 0
    pending: int = 0
    lease: int = 1
    job_source: int = 0
    job_policy: int = 0
    job_lease: int = 0


def transitions(s):
    if s.database:
        yield "enqueue index", replace(s, pending=s.source)
        yield "change source", replace(s, source=2)
        yield "revoke scope", replace(s, policy=2, allowed=False)
        yield "grant scope", replace(s, allowed=True)
        yield "capture summary job", replace(
            s, job_source=s.source, job_policy=s.policy, job_lease=s.lease
        )
        yield "reclaim lease", replace(s, lease=2)
    if s.pending:
        # The vector service can finish a previously issued write after SQL
        # becomes unavailable, or after the source was changed/revoked.
        yield "deliver delayed vector", replace(s, point=s.pending, pending=0, point_project=0)
    yield "wrong-project vector hit", replace(s, point=1, point_project=1)
    yield "database down", replace(s, database=False)
    yield "database up", replace(s, database=True)
    yield "select remote", replace(s, remote=True)
    yield "select local", replace(s, remote=False)


def may_use_candidate(s):
    # Candidate identity is rehydrated/checked against authoritative metadata.
    # Project 0 is this request's explicit scope. No Qdrant excerpt is trusted.
    return bool(s.point) and s.database and s.allowed and not s.remote and \
        s.point_project == 0 and s.point == s.source


def may_publish_brief(s):
    return bool(s.job_source) and s.database and s.allowed and \
        s.job_source == s.source and s.job_policy == s.policy and s.job_lease == s.lease


def check_states():
    initial = State()
    queue = deque([(initial, ())])
    seen = {initial}
    witnesses = {}
    publication_checks = 0
    while queue:
        s, trace = queue.popleft()
        accepted = may_use_candidate(s)
        assert not accepted or (
            s.point_project == 0 and s.point == s.source and s.database and s.allowed and not s.remote
        )
        published = may_publish_brief(s)
        if s.job_source:
            publication_checks += 1
            assert not published or (
                s.job_source == s.source and s.job_policy == s.policy and
                s.job_lease == s.lease and s.database and s.allowed
            )
        # Reject specific tempting weaker protocols, retaining shortest traces.
        if s.point:
            conditions = {
                "trusting stale vector version": s.point == s.source,
                "trusting vector project membership": s.point_project == 0,
                "trusting vector data during SQL outage": s.database,
                "forgetting scope revocation": s.allowed,
                "reusing local memory for a remote request": not s.remote,
            }
            for name, safe in conditions.items():
                # Isolate one omitted gate: every other eligibility gate holds.
                if not safe and all(v for k, v in conditions.items() if k != name):
                    witnesses.setdefault(name, trace + ("naive candidate accepted",))
        if s.job_source and s.database:
            if s.job_lease != s.lease:
                witnesses.setdefault("publishing with an expired claim", trace + ("naive publication",))
                assert not published
            if s.job_source != s.source or s.job_policy != s.policy:
                witnesses.setdefault("publishing against changed inputs/policy", trace + ("naive publication",))
                assert not published
        if len(trace) < 8:
            for name, nxt in transitions(s):
                if nxt not in seen:
                    seen.add(nxt)
                    queue.append((nxt, trace + (name,)))
    assert len(witnesses) == 7
    for name, trace in witnesses.items():
        print(f"COUNTEREXAMPLE {name}: {' -> '.join(trace)}")
    print(f"PASS {len(seen)} reachable states through depth 8; {publication_checks} captured-job checks")


def check_frontiers():
    checked = 0
    naive_witness = None
    for order in permutations(range(3)):
        done = set()
        frontier = 0
        for packet in order:
            done.add(packet)
            naive_frontier = max(done) + 1
            if any(i not in done for i in range(naive_frontier)):
                naive_witness = naive_witness or (order, naive_frontier)
            while frontier in done:
                frontier += 1
            assert all(i in done for i in range(frontier))
            assert frontier == 3 or frontier not in done
            checked += 1
    assert naive_witness
    print(f"COUNTEREXAMPLE maximum-completed watermark: order={naive_witness[0]}, unsafe frontier={naive_witness[1]}")
    print(f"PASS {checked} contiguous-frontier transitions")


def check_token_budgets():
    checked = 0
    for context, mandatory, output in product((8192, 16384, 32768), (1024, 7000, 15000, 33000), (256, 2048, 8192)):
        margin = 512
        room = context - mandatory - output - margin
        if room < 0:
            # A protected passage/request that cannot fit is rejected, not
            # silently shortened or "saved" by dropping authorization framing.
            checked += 1
            continue
        selected = 0
        for optional in (2000, 1024, 768, 512, 256):
            if optional <= room:
                room -= optional
                selected += optional
        assert mandatory + selected + output + margin <= context
        checked += 1
    print(f"PASS {checked} synthetic token-budget cases; mandatory content never truncated")


if __name__ == "__main__":
    check_states()
    check_frontiers()
    check_token_budgets()
    print("LIMIT: finite design model only; no SQL/Qdrant concurrency, tokenizer, performance or recall proof")
