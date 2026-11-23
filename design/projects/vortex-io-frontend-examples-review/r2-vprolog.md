# R2 review: VProlog/Vlog

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594`.

Reference: `design/vortex-io-implementation-plan.md` §10 and the current VProlog/Vlog
implementation.

## R1 findings rechecked

- **Total outcome handling: resolved.** The example returns `IoResult` with the solution, retains
  the tagged terminal value, and labels the exhaustive result presentation as proposed post-query
  syntax rather than a current compiler feature. Its cases include Complete, EOF, Partial, Failed,
  Cancelled, TimedOut, Overflow, MessageTooLarge, Stale and Unknown. The refusal branch separately
  retains its `RejectedOutcomeCell`. This avoids the prior `inspect_io/2` ordinary-goal failure
  path.
- **Deterministic admitted/refused branch: resolved in the companion, consistent with the plan.**
  The companion explicitly says the sum match and `->` lowering are one deterministic driver branch,
  not a speculative Prolog alternative. The implementation-plan §10 VProlog row already requires
  query-driver grammar/lowering for the effect form and forbids effects in speculative
  choice/backtracking. The example makes the special lowering semantics concrete without claiming
  current support.
- **Managed capability fixture: resolved.** The host injects `ReadCap` as a managed capability cell
  binding; the spelling in source is a variable name and grants no authority. This matches the
  plan's owner-minted capability rule.
- **Pending and trail behavior: resolved.** Pending is runner state. The same compiled query, VM
  cursor, bindings and Vlog trail resume and re-await the same request, without re-submitting. This
  aligns with §10 and the shared continuation contract.

## Source check and limitations

Current implementation still does not support the proposed effect/query-driver feature: parser
operators include `->` and `;`, but current logic lowering does not lower these operators, and
`Compiler::solve` is a synchronous solution-vector API with no park/resume cursor. Vlog uses arena
mark/release for logical rollback, which cannot undo external effects. The revised example clearly
marks these as planned grammar/lowering and runner behavior, and distinguishes logical failure from
typed I/O values. No remaining VProlog-specific omission found against the requested R2 checks.

## Verdict

**PASS for VProlog/Vlog at the example-design level.** The four R1 gaps are closed, and planned
behavior is distinguished from current source behavior. Target hash was verified; no target edits
made.
