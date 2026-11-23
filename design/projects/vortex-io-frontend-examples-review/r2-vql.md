# R2 VQL review

Target: `design/vortex-io-frontend-examples.md`, SHA-256
`ce9b9815f379d706588813ea6f4d17b2fc383b6d0408deeae6190eb2f3f4e594`.

## Verdict: pass

The R1 issue is fixed. The proposed `outcome(tagged_fields, full)` pattern exposes the original
managed `OutcomeCell` in each arm, and the example retains it. `Complete` and `Partial` carry
payload/progress; `Failed`, `Cancelled`, and `TimedOut` include optional payload and progress, with
uncertainty shown where defined; `Unknown` retains `lastKnown`. The remaining arms retain full
outcomes with EOF progress, overflow gap, oversize disposition, and stale generation fields.
Admission refusal remains distinct and is retained as a full rejection.

The rest of the VQL path remains aligned with plan §10: the host binds the capability and offset;
the effect occurs once at a script action site, outside row evaluation; `await` is only reached for
an admitted request; Pending parks the same activation and re-awaits the same request; capture stays
an explicit host operation; and staged-result rendering is pure. The sketch clearly identifies its
sequencing/match syntax as proposed M7 grammar, consistent with current VQL's `weave` effect syntax
and lack of sequential match arms. No unplanned VQL runtime or VM behavior is assumed.

Evidence checked: implementation plan §§3.2 and 10; current VQL AST definitions in
`apps/common/xanadu/vql/ast.hpp`; and parser behavior in `apps/common/xanadu/vql/parser.cpp`.
