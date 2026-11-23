# R1 VIcon review

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`2b90cb6a675e12d1ace92a4b222744bc44fa3ce9e4541f8146693c0563034d93`.

Reference: accepted `design/vortex-io-implementation-plan.md` §1, §3.2, and §10 VIcon row; current
VIcon generator/driver implementation.

## Findings

### MAJOR — the sample does not show terminal outcome inspection or preserve full outcome data

The code matches the admission sum, but its admitted arm returns `await(Req)` directly; there is no
subsequent `match` on the await result. The prose says that only `Complete(payload)` permits byte
access and that `Partial(payload, progress)` preserves progress, but it does not show how the source
obtains those fields or how the remaining outcomes retain their status-specific data. In particular,
plan §10 requires payload gating and preservation of progress for payload-bearing `Partial`,
`Failed`, and `Cancelled` results; §3.2 also keeps stale generation and unsupported operation
distinct from logical failure. The VIcon prose enumerates EOF, refusal, failure, cancellation,
timeout, overflow, and unknown, but omits stale and does not establish that progress/optional
payload on failure-like results remains inspectable.

Add a proposed post-await outcome match (or explicitly say the entire tagged outcome cell is
returned unchanged), with `Complete(payload)` and `Partial(payload, progress)` as the only shown
byte-reading cases. Keep EOF, typed failure, cancellation, timeout, overflow, stale/unsupported
refusal, and unknown distinct; retain any optional payload and known progress. This is important for
VIcon because generator exhaustion is already a meaningful language result and must not absorb I/O
statuses.

### MINOR — make fresh submission on generator re-execution explicit

The paragraph correctly says that Pending resumes the same generator and re-awaits the same request.
It does not say what happens if later Icon backtracking re-enters the submit site after that await
has completed. The shared protocol says each re-execution of Submit, including after backtracking,
admits a fresh operation (`vortex-io-implementation-plan.md` §1); this distinction is central in a
goal-directed generator. Add that rule next to the same-request Pending statement so readers cannot
interpret backtracking as replaying an old request or as undoing completed I/O.

## Semantics confirmed

- The yield/stdout boundary is correctly described. Current `write(value)` tees a produced cell to
  the driver (`compiler.cpp:793-822`); the CLI sink prints it to `std::cout`
  (`apps/vicon/main.cpp:41-46, 138-152`). The example properly makes stdout a separate write/flush
  request chain.
- Pending is correctly a parked runner disposition, not a yielded cell or generator exhaustion, and
  resume re-awaits the same request. The plan requires retaining the activation and not resubmitting
  on resume (§1, §10).
- Refusal is kept as a tagged result via the `refused(Why) => Why` arm. The prose also says terminal
  I/O tags stay visible; it does not map them to Icon failure. This preserves the plan's typed
  outcome contract.
- The syntax and driver result are clearly marked proposed. Current `Driver::resume()` is
  `optional<CellRef>` where `nullopt` means the generator ran dry (`driver.hpp:40-42`); the current
  runner is synchronous. No claim of landed async support is implied.

## Verdict

**Revise before calling the VIcon example complete.** The core separation of yield, stdout, Pending,
and refusal is sound. Show the post-await terminal match with lossless status/progress handling, and
spell out that a submit site reached again by backtracking creates fresh work. No implementation or
target edits made.
