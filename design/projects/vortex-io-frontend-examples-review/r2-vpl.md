# R2 review: VPL

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594`.

Reference: `design/vortex-io-implementation-plan.md` §§3.2 and 10, and current VPL
lexer/parser/compiler/executor implementation.

## Verdict: accept

The revised example covers every legal `ReadBytes` terminal variant from §3.2: `Complete`,
`EndOfStream`, `Partial`, `Failed`, `Cancelled`, `TimedOut`, `Overflow`, `MessageTooLarge`, `Stale`,
and `Unknown`. `Conflict` is correctly absent because it is not an allowed read result. `Refused` is
correctly modeled separately as a submission-sum result rather than as an `IoOutcome`. The patterns
retain the full typed result, including progress, optional payload, uncertainty, gap, oversize
disposition, and stale generations; payload access is conditional on a payload-bearing tag.

The example now states equal shape validation before admission and deterministic left-to-right
admission under bounded concurrency. It preserves refused and terminal values at their original
positions, independent of backend completion order. Pending parks at the first unresolved position;
wakeup re-awaits that same request and then scans on. It explicitly says that a fresh execution of
`submit¨` is new work, so resumption does not imply a retry or resubmit.

Current implementation remains accurately scoped as future work: the lexer/parser recognize the Each
adverb, but `lowerEachMonadic` and `lowerEachDyadic` currently refuse it; I/O submit/await, shape
preflight, and resumable VPL execution are not implemented. The example calls the match notation
explanatory pseudocode, so it does not claim these forms are currently executable.

No findings against the requested R2 checks. No target edits made.
