# R2 VLace review

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594`.

References: `design/vortex-io-implementation-plan.md` §§3 and 10; current VLace runner
(`apps/common/xanadu/vlace/lower_io.cpp`, `driver.cpp/.hpp`). Prior findings: `r1-vlace.md`.

## Verification

- **Lazy action and driver mapping are corrected.** The sample now constructs
  `readBytes ReadEndpoint 0 4096` as a pure `IO.ReadBytes` description and separates that from
  `runIOAsync`. The prose maps performance to submission using the host-owned `ReadCap`, then awaits
  the admitted request. This matches §10's pure constructor and perform boundary, while retaining
  §3's managed `Admitted{request} | Refused{outcome}` protocol. Pending resumes the same
  continuation and request; a separate `runIOAsync` call creates a new submission.
- **The read outcomes are lossless and complete for this operation.** The post-performance match
  retains the full tagged value and operation fields; payload access is gated for `Complete`,
  `Partial`, and optional payloads on `Failed`. It preserves refusal separately and lists all
  read-allowed terminal variants from §3.2: `Complete`, `EndOfStream`, `Partial`, `Failed`,
  `Cancelled`, `TimedOut`, `Overflow`, `MessageTooLarge`, `Stale`, and `Unknown`. `Conflict` is
  correctly absent: the operation-result table does not permit it for read/frame/message/decode
  operations.
- **No proposed API is presented as available today.** The prose says `readBytes` and `runIOAsync`
  are plan target spellings, not current VLace syntax or methods; the match is likewise proposed. It
  says current synchronous `runIO` does not implement the path and retains the
  `VLACE_ASYNC_UNAVAILABLE` gate with no blocking `getline` fallback. Current code confirms
  `Driver::runIO` delegates to the synchronous trampoline, which calls `std::getline` and writes to
  the supplied stream.
- **Waiter and request cleanup is now explicit.** Dropping the value or retiring a waiter does not
  cancel or close an admitted request; the request and lease remain owned until backend release, and
  cancellation stays an independent host policy. The separate write/flush/close and abort sequencing
  also remains intact.

## Verdict

**Accept.** The revised VLace example resolves the prior action/submit mismatch, full-read-outcome
coverage, and lifecycle/re-execution ambiguity. Its proposed API is clearly marked as planned, not
implemented or currently available. No target or source files changed.
