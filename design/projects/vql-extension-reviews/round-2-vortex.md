# Round 2 — Vortex/runtime critique

Reviewed current source, R1 shared ledger/peer reports, revised ownership/cancellation/target
contracts, and U4 workflow report. Read-only this round; new regression tests were inspected, not
run or certified.

**R1 closure:** I1's UDF/lambda lexical-state leak is closed by code structure: `BindingScope`
snapshots both maps and swaps them back in its destructor, and its lifetime covers parameters,
defaults, body and return-contract checking. Caller arguments are captured before installing
parameter bindings. I2 is closed by compiler preflight rejecting `AnchorKind::Create`;
dimension-attached creation remains accepted. I personally ran the 16 compiler/conformance tests for
I2 before this round, but that does not certify the newer evaluator changes. V3's
resolved-target/credential checks and U1's durable late-receipt inbox are now explicit proposals. U4
correctly adds an authoritative admission journal, atomic executor/generation claim and
unknown-outcome takeover policy; it does not invent remote exactly-once behavior.

## V4 — Medium implementation bug: failed perspective scope contaminates later commands

`apps/common/xanadu/vql/vql_engine.cpp:695–716` (`executePerspectiveBlock`) still saves/restores
`activePerspective_` manually. The same pattern remains at `:804–815` in `evaluatePerspectiveStep`.
Neither uses the new exception-safe guard. A refused watch, return-contract failure, unknown
function or work-budget exception skips restoration. Subsequent ordinary commands then select or
create the wrong perspective's overlay.

Source-derived reproduction using the existing `VQLPerspectiveTest.ScopedPerspectiveBlock` fixture:

1. Bind `$p` to a paragraph and create Alice's private note using
   `with perspective("alice") { $p/+d.comment => [ "Alice note" ] }`.
1. Verify ordinary `$p/+d.comment` is empty.
1. Execute `with perspective("alice") { watch ## {} }` and catch the expected unsupported-watch
   error.
1. Execute ordinary `$p/+d.comment` again. The leaked `activePerspective_ == "alice"` routes the
   read to Alice's note, although the scoped block already exited exceptionally.

This is presently arena selection/mutation contamination, not evidence of external credential
authority. It nevertheless violates scope lifetime and would be unsafe to inherit into the proposed
host-service execution context.

**Fix:** use an RAII scoped replacement/restoration guard for `activePerspective_` in both block and
qualified-step paths. Nested success and failure must restore the immediate enclosing perspective,
including default/ambient state. Add tests with existing canonical and Alice/Bob overlay values,
failure inside an inner perspective block, and failure in a perspective-qualified function step.
After catching the error, verify both reads and a newly applied morphism use the prior perspective.

No additional actionable gaps found in the revised generic host/admission/replay contracts in this
scoped round. Those integrations remain proposals, with the controlled crash/cancellation/ownership
tests still required. V4 means this reviewer is not yet quiescent.
