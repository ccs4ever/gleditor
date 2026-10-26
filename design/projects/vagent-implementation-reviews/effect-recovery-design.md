# Effect-call recovery grammar review

**Verdict: CONVERGED on the proposed narrow boundary**, with two semantic details to state in the
clarification. The gap is real: §6.6 includes effects in the deadline/recovery contract (lines
862–867), while §8.9 gives `else` productions only to `achieve`, `ask`, and `await` (lines
1316–1321). The existing dangling-`else` note also names only those constructs (§8.9, lines
1337–1340). The frontend audit reproduces the resulting rejection of `let value := read() else 0` in
a plan (`design/projects/vagent-implementation-reviews/frontend-r1.md`, “Specification/API gap”).

Allowing recovery only on a direct call that is the whole plan statement or the complete RHS of `:=`
is a minimal, backward-compatible extension. Reuse `else` and the existing `cause` binding; do not
add a keyword or make every call expression recoverable. The parser can attach the optional recovery
to the root call/assignment statement, then the checker can resolve the call and allow it only when
it names an effect in a plan. This gives the intended forms:

```vagent
let value := read() else 0
value := read() else 0
read() else recover()
```

The last form is for an effect whose result is intentionally discarded. State that recovery is
selected only for the host-reported effect failure or elapsed deadline, and that a value-producing
call's recovery expression has the effect method's declared result type. For a discarded call, the
recovery result is discarded too. The recovery handles the failed wait; it does not promise to undo
an external action that the host may already have performed. This is consistent with V6 (§1, lines
187–190), which runs host effects outside the agent turn and makes cancellation best effort.

The root-statement restriction preserves existing `if` binding. In
`if c then x := read() else x := 0`, the `else` remains the `if` branch because the call is nested
inside the `if` expression; to recover the effect, write
`if c then { x := read() else 0 } else { x := 0 }`. Apply the same boundary to `while` and `for`
bodies: a recovery inside their expression body requires a block. Do not let an `else` attached to a
nested initializer or other subexpression consume a token at the enclosing block level.

The checker must reject recovery on a non-effect `def`/capacity call, on a handler call (handlers
stage effects and receive `EffectFailed`, rather than suspending; V6 and §6.7 lines 899–900), and on
any call outside a plan. It must also reject suspension/recovery through `<-`, an initializer
containing an `if` or block, or a nested operand. Those positions remain excluded by §6.6's
whole-state continuation rule (lines 878–885); only the direct statement call is newly covered.
`achieve`, `ask`, and both `await` forms keep their current nearest-unmatched-`else` behavior.

## Required specification edit

Add a statement-level recovery production/exception for the three direct forms (bare call,
`x := Call`, and `let x [: T] := Call`), with the parser's attachment restricted to the root
statement shape. Update §8.9's dangling-`else` note to include this plan-only effect case and its
braced-branch rule, and qualify §6.6's table/recovery sentence as plan suspension behavior; keep the
separate handler behavior explicit. No general `Postfix [else Expr]` production is safe: it would
steal the `else` in the `if` example above.

## Disposition

The grammar boundary is sound and adds no new syntax category beyond the existing `else` recovery.
It closes the documented/API gap without broadening continuation support. The two clarifications
above—result typing/discard behavior and recovery's non-rollback of external effects—make the
contract precise; neither changes the proposed parser shape.
