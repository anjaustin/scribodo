# AUDIT-005 — initial findings and reproduction

**Date:** 2026-11-25  
**Tree:** `69b98abf1ecf86c560b435a209327175c182eab1`  
**Report:** [AUDIT-005](../docs/AUDIT-005-repository-audit.md)

No product changes were made. The working tree was clean before and after the initial audit.
Documentation was added only after the author requested a durable record.

## Commands and retained evidence

- `SCRIBODO_EVIDENCE_SKIP=1 SCRIBODO_YARDSTICK_SKIP=1 make test`: exit 0.
  Raw output: [test.log](audit005/test.log).
- `make analyze`: exit 0.
  Raw output: [analyze.log](audit005/analyze.log).
- `make dependency-report`: make exit 2; results and full returned digests are recorded in the report.
  Network findings are point-in-time observations, not an offline reproducibility guarantee.
- The probe source is [probe.c](audit005/probe.c). It links the same core and LLM objects as the headless
  suites, creates its own temporary file-backed project, makes no model requests, and does not connect
  to a real project database. It retains its isolated fixture for inspection.
  The retained source was compiled and rerun successfully after documentation was added; output is
  [probe.log](audit005/probe.log), with the same five diagnostic observations.

From the repository root, compile and run with:

```sh
make --no-print-directory -s -f Makefile -f - audit-probe <<'MAKE'
audit-probe: $(CORE_OBJS) $(LLM_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) journal/audit005/probe.c $(CORE_OBJS) $(LLM_OBJS) -o /private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_audit_probe $(MARIADB_LIBS) -lpthread
	/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo_audit_probe
MAKE
```

The output location above is this session's approved temporary directory. On another machine, replace both
executable paths with a writable temporary path. The probe itself uses `TMPDIR`, falling back to `/tmp`.

## Observed diagnostic output

```text
failed file message: save_failed=1 window.unsaved=0
first retry: n=1 ok=1 save_failed=1
second retry: n=1 ok=1 persisted_messages=2
distinct names (203 bytes): writes=1,1 history_A=2
newest A revision contains: document B private text
```

The original temporary fixture was
`/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode/scribodo-audit-g3W9eU`.
It is ephemeral and is **not** the durable evidence: the source and captured observations above are.
The retained probe differs from the original only in selecting its fixture directory portably and checking
store/session allocation before use.

## Interpretation and limitations

The probe deliberately sets the same failed-save flag as the failed append callback. It proves the resulting
guard state, not a disk-full event through the real UI. UI consequences were traced through the New/open/quit
call sites. The duplicate append and cross-document revision reads were executed through product APIs.

The existing `make test` and analyzer both passed despite these behaviors. No new automated regression test
or remediation is claimed. Sanitizer, service-backed, crash and UI suites remain unrun for this audit.
