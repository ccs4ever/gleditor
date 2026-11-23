# R2 VIcon review

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594`.

Reference: the VIcon passage in the frozen target; `design/vortex-io-implementation-plan.md` §1,
§3.2, and §10; current `apps/common/xanadu/vicon/compiler.cpp`, `driver.hpp`, `driver.cpp`, and
`apps/vicon/main.cpp`.

## R1 findings checked

- **Outcome inspection and retention: fixed.** The updated passage now matches the `Seen` value and
  lists every read outcome from the plan: `Complete`, `EndOfStream`, `Partial`, `Failed`,
  `Cancelled`, `TimedOut`, `Overflow`, `MessageTooLarge`, `Stale`, and `Unknown`. Each branch
  retains the full managed outcome; optional payload and progress are available on the variants that
  carry them. Payload access is status/presence-gated. Admission refusal also retains the full
  rejected outcome.
- **Backtracking versus Pending wait: fixed.** Pending parks the same generator cursor/token and
  wakeup re-awaits the same request without repeating Submit. Separately, a backtracked visit that
  reaches Submit again creates a fresh request, and backtracking cannot undo the external effect.
  This matches plan §1.

## Other contract checks

- **Yield versus stdout:** The example correctly says current `write(value)` yields to the host and
  does not itself write bytes. It treats rendering as observational and makes stdout's write and
  flush a separate request chain. Current source confirms the boundary: `Compiler::emitCall` tees a
  value to the driver (`compiler.cpp:793-822`), while the CLI sink writes `std::cout`
  (\`apps/vicon/main.cpp:41-46, 138-152).
- **Effects versus generator failure:** `Refused{fullRejectedOutcome}` and terminal I/O statuses are
  retained as tagged values. They are distinct in the sketch from `Failed(Error)` in the proposed
  driver result, which reports runner failure, and from generator exhaustion (`Finished`). The plan
  says VIcon/VLace return refusal as ordinary typed values (§3.2).
- **Current versus proposed API:** The text clearly labels the grammar and outcome wrapper proposed.
  Current `Driver::resume()` is synchronous and returns `optional<CellRef>` with `nullopt` for a dry
  generator (`driver.hpp:40-42`); the example does not imply async support has landed.

## Verdict

**CONVERGED.** Both R1 issues are addressed, and the example preserves the VIcon yield, runner,
wait, external-effect, and outcome boundaries required by the plan. No target or implementation
edits made.
