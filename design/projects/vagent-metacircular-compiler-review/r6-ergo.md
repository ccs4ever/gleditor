# R6 critique — Programming-language design & ergonomics
Tally: 0 BLOCKING / 0 MAJOR / 7 MINOR. VERDICT: CONVERGED

Both R5 majors verified fixed against source:
- as_body sound-by-construction (re-runs matchAttach's own predicate,
  meta.cpp:1751-1773,1912-1932); Code<()> confirmed the ONLY invariant
  Code param (checker.cpp:1002,1006,1011); wildcard arm fires only on
  unparameterized expected (:665-674); TypeMismatch pinned by
  vagent_mutation_test.cpp:307-317.
- registration Str 3-state matches codebase status conventions
  (stageOutcome "achieved"/"failed"/"dropped" executor.cpp:3007,2727;
  no enum/sum types in language).
- Deferred mechanics verified: marks>=2 in-executor + post-commit
  marks==0 point (executor.cpp:252-257,3024-3029); registerProgramOps
  partial commit (vortex_vm.cpp:96-123); silent-Nop (:1659-1660);
  Opt bound-read unwrap (checker.cpp:96-100,1904,1912,1920).

Minors folded this round:
m1 Q3 wording — graft_body reachable only via as_body + bound-read.
m2 batch register([Cell]) undeclarable (name-keyed sigs, no overload/
union) → register_all distinct name chosen; metaForm-arm/any-param
rejected.
m3 unlabeled heads → single shared fault: tombstone key → +d.name
required, refuse at request time.
m4 drain commit semantics — two-pass per-head isolation; same-unit
callee fault faults dependents transitively; sync-refusal vs drain-
tombstone phase split stated.
m5 registration divergence re-attached — committed-vs-map divergence is
restore window + no-backing session entries; pending window has both
absent.
m6 committed = carrier-listed OR program-image registered
(registerProgramImage ops enumeration); already-committed head →
idempotent handle return.
m7 bundle — as_body does descriptor-internal check only (clause-formal
stays at install); dim_named/as_body pure; test/guard slots wording;
Opt unwraps on bound-name/field/index; §11.1 cited.
