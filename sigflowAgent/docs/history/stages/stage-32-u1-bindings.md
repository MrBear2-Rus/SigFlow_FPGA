# Stage 32: U1 teaching bindings and trusted service

Date: 2026-10-21. Implements the U1 association portion of
[the stage 32–34 plan](u1-u2-stage32-34-plan.md).

## Ownership and durable association

`agent/src/sigflow_edu_agent/teaching_store.py` owns a separate SQLite journal.
It records external session/run IDs, learner/project/revision scope, hashes of
idempotency keys and requests, certified evidence identifiers, and the U2
`TeachingHandle`. It does not copy original questions, teaching cards, tokens,
checkpoints, or U2 workflow state. The instance identity is pinned in metadata;
opening the same database under another identity is rejected. Factory filenames
also include a digest of the trusted bootstrap instance identity.

Session keys are scoped to the learner; run keys are scoped to the external
session. Reuse requires the same request digest and resolved scope. Changed
inputs conflict, foreign owners are denied, and capacity limits reject new
associations without evicting existing work. A handle can only attach to its
original owner, exact evidence scope, intent, and internal U2 session.

`TeachingService.start` commits a stable run reservation before calling U2.
The adapter derives the U2 create key from the external context. A crash after
U2 creation but before handle attachment therefore requires an explicit repeat
of the identical create request. It reuses the external run and internal U2
session; it does not replay background model calls or Jobs. An unattached run
projects `StartRequiresRetry`. A reopened U2 session can require explicit
`recover`, with old cards quarantined. Graceful closure during active work may
instead leave the session cancelled, as determined by U2.

No Gateway, adapter, or Runtime operation executes while a binding-store
transaction is held. U2 remains the sole owner of actions, version checks,
budgets, checkpoints, cancellation, and recovery. Cancellation checks ownership
but deliberately remains available after the current engineering revision has
changed or the Gateway is unavailable.

## Verification

Directed command, executed with `sigflowAgent/.venv/Scripts/python.exe`:

```text
.superpowers/sdd/u2-stage28-31/run_u1.py u1-composition-directed test_teaching*.py
```

Result: **16 tests passed, zero failures/errors/skips**, including real U2
Runtime restart/recovery and handle-publication interruption. The service suite
contains an interrupted-publication retry across reopen proving one external
`run-1`, one U2 session, and `RecoveryRequired` after a committed waiting state.
Logs and the machine summary are under
`.superpowers/sdd/u2-stage28-31/u1-composition-directed.{log,json}`.

These service tests use explicitly synthetic engineering evidence. Real Core /
Gateway acceptance and full original U1/U2 regression are tracked by stage 34;
this result alone is not evidence of their completion.
