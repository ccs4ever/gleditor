# R1 VLace review

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`2b90cb6a675e12d1ace92a4b222744bc44fa3ce9e4541f8146693c0563034d93`.

References: `design/vortex-io-implementation-plan.md` §§3 and 10; current VLace lowering and runner
(`apps/common/xanadu/vlace/lower_io.cpp`, `driver.cpp/.hpp`, `lower_effects.cpp`).

## Findings

### MAJOR — VLace uses the wrong planned action entry

The example says it follows the shared cell protocol but shows
`performIO (submit ReadCap (read_bytes 0 4096))`. Plan §10 instead specifies
`readBytes endpoint offset max` as a *pure constructor* of a lazy IO value, with `IO.ReadBytes`
admitted only when `Driver::runIOAsync(action)` performs it. The same plan section says every
frontend binds and pattern-matches the managed `Admitted{request} | Refused{outcome}` submission sum
before await. The sample neither constructs the planned `IO.ReadBytes` action nor shows that sum
being matched; its prose makes `performIO` implicitly consume both submit and await.

Show the pure `readBytes` action and the runner boundary separately. State where the managed
submission sum is inspected and how the runner performs/awaits the admitted request while preserving
refusal as a value. If the intended VLace bridge deliberately combines those steps into `performIO`,
specify that wrapper as a frontend adaptation of the §3/§10 protocol rather than making the example
look like the generic `submit` call itself is an IO constructor.

### MAJOR — the example omits terminal outcomes and fields

Plan §3.1 defines `Rejected`, `Complete`, `EndOfStream`, `Partial`, `Failed`, `Cancelled`,
`TimedOut`, `Overflow`, `MessageTooLarge`, `Stale`, `Conflict`, and `Unknown`; §3.2 also makes
progress and optional payload meaningful on several failure-like outcomes. The VLace paragraph calls
outcomes ordinary typed values and names complete, partial, EOF, failure, cancellation, timeout,
overflow, and unknown, but omits `MessageTooLarge`, `Stale`, and `Conflict`. It does not say that
the original outcome, progress, and any permitted payload are preserved for the failure-like
variants. The sample has no post-await match showing payload gating.

Add a proposed outcome match or explicitly state that VLace returns the unmodified tagged
`OutcomeCell` for inspection. Keep `Complete` and payload-bearing variants as the only
payload-access cases; retain progress and optional payload on `Partial`, `Failed`, `Cancelled`, and
`TimedOut`, and keep `MessageTooLarge`, `Stale`, `Conflict`, and `Unknown` distinct. Admission
`Refused{Rejected}` should remain separately inspectable from an admitted request's terminal
outcome.

### MINOR — clarify the lifecycle when an activation is abandoned or an action is performed again

The shared fixture correctly says host teardown retires a waiter and that cancellation is a separate
policy choice; the VLace paragraph correctly says Pending resumes the same continuation and
re-awaits its request. It does not state that retiring the waiter leaves an admitted request and its
lease alive until backend release is known, nor that re-performing/re-executing the action is a new
submission. Plan §§1, 3 and 10 require request records to outlive arena marks while backend-active,
cancellation to be a request rather than proof of stopping, and a repeated Submit site to admit
fresh work.

Add this lifecycle/re-execution distinction to the VLace text or explicitly point to the shared
host-fixture rule. This prevents dropping a lazy action, retiring a waiter, or resuming Pending from
being mistaken for close, cancel, or a fresh submission.

## Semantics confirmed

- The construction/performance distinction is sound: the example identifies `performIO` as a planned
  runner boundary and does not claim that current VLace grammar implements it.
- Its explicit `VLACE_ASYNC_UNAVAILABLE` gate and rejection of blocking `getline` fallback agree
  with plan §10. Current `Driver::runIO` is synchronous (`driver.cpp`); `runIoOnVm` directly calls
  `std::getline` and writes to the supplied stream (`lower_io.cpp`). Current builtins are `pure`,
  `bind`, `then`, `putStr`, `putStrLn`, and `getLine`; there is no `readBytes` or async driver
  result. The example accurately describes this as migration work.
- Write, flush, graceful close, and explicit abort are correctly kept as separately submitted
  operations with individually inspected outcomes when asynchronous output is introduced. Dropping
  the lazy value is not treated as successful close.
- Shared fixture statements correctly keep capabilities host-supplied and preserve leases until
  backend release is known; the example introduces no VLace-only request/outcome model.

## Verdict

**Revise before calling the VLace example complete.** Keep its explicit lazy / perform distinction,
but align the expression with the planned `readBytes` action and show or promise lossless handling
of the entire managed submission and terminal-outcome protocol. Clarify waiter/request lifetime and
fresh re-performance. No target or source files changed.
