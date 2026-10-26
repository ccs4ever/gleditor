# Vagent scalar compiler review — R1

**Verdict: CONVERGED.** Reconsideration against the full transaction contract withdraws the original
MAJOR finding. This review covers the scalar compiler snapshot read before the compiler agent's
block/`let`/`if` extension. The shared Vortex runtime review was completed separately and converged.
Parent reported the 23 scalar compiler tests passing; I did not rerun them.

## Reconsidered caller-choice case: expected enclosing rollback

At `apps/common/xanadu/vagent/compiler.cpp:444-447`, `:=` and `<-` both lower to a `Value` copy,
while preflight permits `:=` only at a bounded position (`compiler.cpp:275-281`). The statement
wrapper records its starting depth and cuts only choices opened by that statement
(`compiler.cpp:494-513`). A caller's mark remains live.

The initial review treated that as a defect: compile `value := 3` with no choices, set `value` to 5,
then push a caller choice point on the statement cursor and run the graph. Backtracking the caller
choice restores 5. Read with the full contract, that caller mark represents an enclosing
transaction. §5.8 says field writes use trailed `Value`; §6.7 requires a failed handler and its
nested work to leave no trace; §4.2 requires turn rollback. Such enclosing rollback must undo both
assignment forms. In §8.3, “irreversibly” distinguishes `:=` from `<-` inside the bounded
statement's own search: preflight bars `:=` in speculative expression positions, and the wrapper
cuts that statement's alternatives after its first result. The compiler preserves the caller's
choice depth so the enclosing scope can still roll back its transaction. The behavior is therefore
expected, not a compiler defect. The test at `tests/xuzz/vagent_compiler_test.cpp:223-239` correctly
verifies that reversible `<-` is restored by an enclosing caller choice.

Documentation clarification recommendation: make §8.3's boundary explicit, e.g. “`:=` is
irreversible with respect to alternatives within its bounded statement; enclosing handler/block
failure and turn rollback still undo it.” No untrailed write or change to the runtime is warranted.

## Other reviewed behavior

I found no other actionable issue in the scalar snapshot. Preflight rejects unsupported syntax and
binding/type mismatches before graph minting; literal range handling includes the signed minimum
special case; search lowering distinguishes success from false-valued results; comparisons preserve
the right operand on success; the division guards route zero and `MIN / -1` to ordinary search
failure; and `<-` writes are covered by statement marks. These checks were read against §§8.2–8.3
and the Vortex behavior. The compiler agent is extending syntax, so these observations apply only to
the snapshot named above; the new block/`let`/`if` code needs a follow-up review after integration.
