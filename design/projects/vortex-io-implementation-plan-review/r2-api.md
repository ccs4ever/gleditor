# R2 — API designer review

Target: `design/vortex-io-implementation-plan.md`  
Target SHA-256: `ec5d3722af8be1d433bb9e535141184da7c10ca4df4ee6afb050c7d58112ac17`  
Disposition reviewed: `design/projects/vortex-io-implementation-plan-review/dispositions.md`  
Verdict: **ACCEPT WITH ONE MINOR**

## R1 disposition verification

The R1 API findings are resolved in the revised contract:

- Native `RequestId` is private; `PendingAwait` now carries only continuation/waiter, and the worker token is scoped to `BackendCommand`/`NativeCompletion` (§§3.1, 4–5).
- Open uses `admitOpen` and the shared admission, await, cancellation, budget and terminal path; endpoint/capability cells are minted only on successful owner publication (§4).
- Waiter cancellation and request cancellation have separate result types; request cancellation returns accepted/already-terminal/refused, with eventual progress observed by awaiting the original request (§4).
- Capture has prepare, `appendRequest`, awaitable common service admission, guarded commit, and explicit recovery outcomes (§8.2). The duplicate generic Capture operation is removed (§3.1).
- Admission refusal is distinct from terminal `IoOutcome`; the result matrix defines payload/result behavior, closed error codes cover malformed/truncated data and cursor conflicts, and all frontend paths use the managed `Admitted | Refused` sum (§§3.2, 10).
- Buffer policy now names line/frame/message fields and distinguishes refusal from post-read oversize disposition (§3.1). Owner cell wrappers, allocation incarnation APIs, authoritative fields and private-ID classes are inventoried (§§3.1–3.2).

The disposition’s direct-Vortex/table-count correction is clear: Direct Vortex is explicitly the shared-host baseline and “not counted among the six bridges” (§10). Freezing the `d.io.*` names is also coherent with the implementation plan’s stated role of resolving the source redesign’s spike choices; M0 now validates the graph rather than leaving the public names open.

## Finding

### MINOR — Capture commit should explicitly bind the supplied successful outcome to this capture’s append request

**Evidence:** §3.1 makes the `Capture` graph authoritative for its `append request occurrence` (line 94). §8.2 exposes `commit(CaptureCell, OutcomeCell successfulAppend)` and says it accepts only a successful terminal append (lines 467–487), but does not explicitly state that the outcome must be the terminal outcome of the exact append request linked from that same `CaptureCell`.

**Why it matters:** A successful append outcome from another capture must not authorize publication of this capture’s interval translations. The graph gives the implementation the needed association, but the invariant should be normative at the public method boundary.

**Required fix:** State that `commit` resolves the append request occurrence from `CaptureCell`, verifies matching request identity and successful terminal status, and refuses any unrelated or stale outcome. No API redesign is needed.

## Tally and verdict

- Blocking: 0
- Major: 0
- Minor: 1
- Nit: 0

Acceptable to proceed. The remaining finding is a local identity-validation clarification, not an unresolved cross-layer API or sequencing decision.
