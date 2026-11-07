# R5 critique — Programming-language design & ergonomics
Tally: 0 BLOCKING / 2 MAJOR / 6 MINOR. VERDICT: NOT CONVERGED

B1 follow-up: deadlock genuinely closed — mint→register→Code<Cell>→run and
Code<Cell>→wildcard-Code install verbs all type-check end to end (verified
checker.cpp:1178-1181, 665-674, 999-1013, 96-100,1729; meta.cpp:2206-2221,
1833-1835; vortex_vm.cpp:982-999; executor.cpp:252-257,3023-3032).

MAJOR-1: Code<T> invariant — bodyCode params declare Code<()>
(checker.cpp:1000-1011, wildcard arm fires only on unparameterized expected,
:665-674), so register/code_named/body_of-vended Opt<Code<Cell>> type-refuses
graft_body/learn_body and body slots of plans/handlers — Q3's named first
consumer unreachable. Options: (a) relax params to bare Code; (b) as_body
descriptor-checked narrow Code<Cell>→Code<()>; (c) scope v1 to wildcard verbs.

MAJOR-2: kind_registered reading carrier∪pending∪committed reports true
through the entire universal pending window while run fails closed —
RunCall consults opcodeMap_ only. Also: E1-iv drain callee check needs
pending∪committed or same-turn A→B faults. Fix: committed-only Bool or
registration(h): absent|pending|committed; state which set E1-iv consults.

m1 drain-fault carrier-listing ambiguity — if faulted entry pins, retry is a
no-op via the membership-skip. m2 batch return type/fault granularity
unwritten. m3 registration/dim_named need reserved-name arms + purity;
dim_named signature unpinned. m4 no worked example; run(register(h))
type-refuses (Opt operand) — bind-first-run-next-turn idiom. m5 fault:<label>
record home ambiguous. m6 Code<Cell> result overstates honesty —
refTypedResult is descriptor-driven (executor.cpp:695-716); Cell-typed value
can hold a non-position.
