# R1 review: VPL

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`2b90cb6a675e12d1ace92a4b222744bc44fa3ce9e4541f8146693c0563034d93`.

Reference: `design/vortex-io-implementation-plan.md` §3.2 and §10, plus current VPL
lexer/parser/compiler/executor and `VplView`.

## Findings

### MAJOR — outcome examples do not preserve all terminal outcome fields

The explanatory match lists `Complete`, `Partial`, `Refused`, `EndOfStream`, `Failed`, `Cancelled`,
`TimedOut`, `Overflow`, and `Unknown`, but omits `MessageTooLarge`, `Stale`, and `Conflict`. The
plan's shared outcome union includes all three; `Stale` is valid for a read, and its
payload/progress rules require retaining fields for payload-bearing failure, cancellation, and
timeout, as well as `Unknown.lastKnown` progress. The shown bare
`Failed | Cancelled | TimedOut | Unknown` patterns do not demonstrate that progress and optional
payload remain observable. Extend the outcome sketch to cover every legal read terminal and preserve
each tag's full typed fields; gate payload access to tags that carry one. `Refused` is a submission
sum arm, not an `IoOutcome` terminal, so label it separately from the terminal-outcome match.

Evidence: implementation plan §3.2's `IoOutcome` union and read/frame/message allowed terminal
variants; §10's requirement that VPL retain a shape-preserving sum at every position and that
terminal outcomes remain values.

### MINOR — admission scheduling constraints should be visible in the example

The prose promises shape preservation, pre-admission shape validation, and position stability, but
does not state §10's deterministic left-to-right admission with bounded concurrency. Add that
constraint where the example describes `submit¨`; otherwise the sketch leaves submission
fanout/scheduling open even though the accepted plan settles it.

Evidence: implementation plan §10 VPL row.

## Semantics and syntax checked

- The shape fixture is explicit (`Caps` and `Specs` are equally shaped host-supplied views), and the
  mismatch is rejected before any submission. It avoids implying source programs construct
  capability authority. The request/result position and out-of-order completion behavior match the
  plan.
- The parked carrier behavior is sound: one active waiter, first pending position, same request
  re-await after wake, then continued left-to-right scan. A fresh execution of `submit¨` is
  explicitly identified as new work, so there is no implied retry.
- The example properly calls the match notation explanatory pseudocode. Current VPL lexer and parser
  recognize `¨`/Each, but current `lowerEachMonadic` and `lowerEachDyadic` refuse with
  `VPL VM each adverb is unsupported`; `submit`/`await`, managed I/O sums, shape preflight, and a
  resumable `Executor` are also not implemented. The companion document accurately identifies these
  as bridge work, though it should make clear that Each itself is not currently executable.
- The supplied frozen target hash matches. The current implementation-plan file hash is
  `9226c1c887c993da9f7f2d4845cd3ecca20082b8c5f63cde68719e5308ff8a0e`, the link-only revision
  recorded in the review README; the README records the accepted plan hash separately, so this
  review uses the stated frozen companion target and normative §10 content.

## Verdict

**Revise before calling the VPL example complete.** The shape, waiter, resume, and no-retry
semantics align with §10. Add the omitted legal outcome variants and preserve full progress /
optional payload data, distinguish admission refusal from terminal outcomes, and state the
left-to-right bounded admission rule. No target edits made.
