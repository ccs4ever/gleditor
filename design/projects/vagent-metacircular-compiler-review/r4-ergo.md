# R4 critique — Programming-language design & ergonomics
Tally: 1 BLOCKING / 4 MAJOR / 6 MINOR. VERDICT: NOT CONVERGED

B1 (BLOCKING): No Cell→Code surface exists; D1 milestone untypable.
run's operand requires Code (checker.cpp:1178-1181, compile-time error on
Cell); graft_*/learn_* params are Code (checker.cpp:999-1013); M3 bars the
silent Cell→Code upgrade; every producer yields Cell; d.code membership is
reachable only inside learn_* (meta.cpp:2206-2221) which takes Code — double
deadlock. Fix: register(h: Cell): Opt<Code<Cell>> atomic — enumerate+validate+
kind-register+d.code-file+return the handle code_named vends; dissolves the
reify-ordering trap by making it unexpressible.

M1: register has no surface spec — param type, return, purity, refusal
channel (host-step is bool→statement-failure, drops text; program-visible
causes are a fixed vocabulary), batch form, reserved-name arm (import
std:meta precedent, checker.cpp:645-646,851-857 — a user def register
shadows the builtin otherwise), carrier dim name unspelled, kind-vs-code
naming collision.

M2: deferred register lies to the caller — register(h);run(h) under marks →
RunCall failJump → produced=false, indistinguishable from computing nothing;
§8.5 mark-bounded score loop can't evaluate compiled candidates; no
registered(h)/pending(h) probe.

M3: writer/linker exposure unresolved either/or — d.code+run() makes them
public unguarded Link/Value primitives (ungated graft channel bypassing
graftable gates; parent §8.5 calls it "§11's unguarded door"); signatures
unwritten (dim must be the dim CELL via run's marshaling — a name Str
toInts to 0 → silent noCell; no dim_named surface exists); writer→hard
kindImmutabilityFault vs linker→soft noCell asymmetry undocumented.

M4: R3 "runtime ceremony feeds it source text" contradicts §5's never-raw-
text unless host-driven — VProlog assert/consult precedent (host compiles
source at the boundary, program never evals).

MINOR m1-m6: run-fail conflates deployment/domain failure; D5 refusal
surface unwritten + register doesn't police D5 (emitter contract); validate
declared pure but mints +d.validate residue; D7 "or bare Code" conflates
wildcard with value type (Code<Cell> exactly); no worked programming-model
surface; reify silently drops kind-registration (dead Code value).

VERDICT: NOT CONVERGED
