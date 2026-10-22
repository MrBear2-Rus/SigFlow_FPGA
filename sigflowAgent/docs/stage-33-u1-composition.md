# Stage 33: U1 in-process U2 composition

Date: 2026-10-21. Implements the HTTP/bootstrap portion of
[the stage 32–34 plan](u1-u2-stage32-34-plan.md).

## Transport and deployment boundary

`--u2-teaching` loads `teaching_factory.compose` into the existing U1 server.
The base package keeps its Python 3.9 contract and does not import U2 unless the
extension is enabled. The extension requires Python 3.11+ and the independently
installed `sigflow_u2` distribution (or the trusted source-tree launcher). A
missing dependency or invalid trusted configuration fails before readiness.
U2 does not create a second HTTP listener or readiness line.

The extension routes live under `/api/v1/u2/sessions` and `/api/v1/u2/runs`.
They use the existing `edu.api.v1` envelope and explicitly identify their data
as `u1.u2-teaching.v1`. Host, Origin, and Bearer checks precede extension dispatch.
Exactly one valid `X-Learner-ID` is required; create operations also require one
`Idempotency-Key`. The learner header is an assertion by the trusted UI holding
the UI token, not independent learner authentication.

Starts/actions/cancellation return 202. Reads return 200; a teaching wait is
pending with `run=null`. Frozen teaching-card schemas apply to projected cards;
a frozen agent-run is returned only for a completed workflow. Events expose
external session/run identity and sequence, action, version, and status metadata
without internal U2 IDs, question/card bodies, or raw runtime results. The event
format is an extension, not the frozen agent-event format. Cursors are bounded
canonical decimal strings. State versions are validated by the U2 adapter.

Ordinary requests cannot supply implementation paths, database paths, factories,
material providers, or reference authority. HTTP reference/L4 remains denied.
ReferenceProvider, receipts, and teaching memory can only be provided through
trusted factory arguments; this stage does not implement U3.

## Evidence and lifecycle

The factory takes identity, paths, and credentials from bootstrap. It checks
Gateway identity and the saved, synchronized current project revision. Report
project/revision/snapshot scope must match before evidence certification by
`GatewayEvidenceCatalog`; projection resolves anchors from allowed Jobs, adding
only verification results already committed by U2. Current revision checks
surround preparation and card/event reads. This is an observation boundary,
not a global lock on concurrent project edits.

The service closes U2 workers, binding storage, and all registered resources.
One failed close no longer prevents subsequent owned resources from being
released; the first error is re-raised after all closers are attempted. Bootstrap
now places readiness emission and self-test early return inside cleanup, so a
broken stdout or `exit-after-ready` releases both the bound socket and teaching
resources. Verification-adapter close runs in a daemon with a bounded join to
avoid an unbounded shutdown wait behind external I/O.

Response secrets include bootstrap and configured model secrets. U2 projection
also validates safe public output. No raw initialization exception is written
to readiness stdout.

## Verification and remaining integration

`u1-composition-directed` ran all `test_teaching*.py`: **16 passed, zero
failures/errors/skips**. This includes an actual bootstrap subprocess, one ready
line, same-port capabilities, authenticated shutdown, original-route coexistence,
Host/Origin/Bearer/owner refusal, pending/terminal reads, explicit actions,
cancellation, replay, cursor checks, and restart recovery.

New lifecycle regressions were first observed failing in
`u1-lifecycle-red2` (two failures, zero errors), then passed in
`u1-lifecycle-green` (seven service tests). They exercise a real listening socket
and the real binding database. The crash-window retry test uses a real U2
Runtime and persistent journals. Evidence fixtures remain synthetic; stage 34
owns the real engineering chain, full regression, and delivery matrix. Missing
real UA-10, material/receipt/teacher input, other detectors, and UI/platform
acceptance remain external gaps and are not claimed complete here.
