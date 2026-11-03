# E1 implementation review, round 1: compiler and codebase expert (persona codebase)

Scope: commit `0dc6bbad` (`compiler.cpp` to new `compiler_gen.hpp`), the worker handoff
`/tmp/p19-e1/handoff.md` and the IR-dump harness under `/tmp/p19-e1/harness/`, against slice E1 of
`remaining-plan.md`. Code at HEAD `1140853d` (E1 is unchanged by the later docs-only commit).
Scripts and probes: `/tmp/p19-codebase/` (`chk.py`, `t1..t5.cpp`, `flags.txt`).

## What is sound

- **The move is mechanical.** `chk.py` compares `compiler.cpp` lines 93-1278 at `3b86b36d` with the
  body of `codegen` in the header, ignoring blank lines. The differences are exactly the four
  `using` lines the header adds, plus `inline` on `bail`, `unsupported`, `tyOf`, `S`, `D`, `Hd` and
  `collectCallees` (clang-format rewrapped two signatures). No logic changed.
- **The rest of `compiler.cpp` is untouched.** The diff has 1191 deletions: the 1186 moved lines
  plus `namespace {`, `} // namespace` and the three lines of `compileAgent` that now say
  `codegen::Gen` / `codegen::Bail`. The additions are the include, `namespace codegen {`, its
  closing comment, and those same three lines. The out-of-class `Gen::` members and the file-local
  `static` helpers (`isCompoundStmt` at `compiler.cpp:2136`, `hasDynamicCall` at `:5191`) stay in
  the file and, being `static`, keep internal linkage.
- **No name collides today.** Nothing else in `apps src include tests tools` defines
  `xanadu::vagent::runtime::codegen`. The other same-named types are `Parser::Bail`
  (`parser.hpp:69`), `Parser::Container` (`parser.hpp:74`), `vlace::Ty` and the bounded frontend's
  local `Local` (`vagent/compiler.cpp:681`). The five names `checker.hpp` declares in `runtime`
  (`FieldInfo:135`, `QuestionInfo:178`, `MethodInfo:185`, `GoalInfo:205`, `PlanInfo:212`) do not
  clash: I compiled `t2.cpp`, which includes `checker.hpp` then `compiler_gen.hpp` and uses
  `codegen::FieldInfo`, `runtime::FieldInfo` and an unqualified `FieldInfo` inside `codegen`. It
  builds with the production flags under clang++ and under g++ (no warnings, `-Werror`).
- **Header hygiene.** Include guard follows the repository spelling
  (`COMMON_XANADU_VAGENT_RUNTIME_COMPILER_GEN_HPP`); a file comment says why the header exists; the
  header compiles standalone (`t1.cpp`, 6.7 s, mostly `vortex_vm.hpp`); double inclusion is clean
  (`t3.cpp`). The `using` declarations sit inside `codegen`, and there is no `using namespace`
  anywhere in it. There is no anonymous namespace and no namespace-scope `static`, so nothing in the
  header has internal linkage except the three constants (m1). All free functions are `inline` and
  the one function template is implicitly fine. `Gen`'s `static constexpr` members are implicitly
  inline. A probe object that includes the header and defines one function (`t5.cpp`) exports only
  that function: no definition from the header leaks as a strong symbol.
- **ODR for the types.** All moved types are complete classes defined identically in every TU that
  includes the header, so linkage change from internal to external is legal. The out-of-class
  `Gen::` members now have external linkage and are defined once, in `compiler.cpp`.
- **Proof.** `base.out` and `new.out` compare equal (`cmp`, 304,449 lines). The dump covers, per
  agent, every arena cell (text bytes, value kind and bits, flags, birth and last op) and every
  link, plus the entry refs of `CompiledAgent`, so any change in emitted code, in constant interning
  or in emission order shows. 14 of 41 compiled agents have arbitrated predicates, and 27 refusals
  dump their code and detail.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### m1. Three namespace-scope constants are `constexpr`, not `inline constexpr`

`compiler_gen.hpp:93-95` (`kPos`, `kNeg`, `kEventFieldOfTurn`) have internal linkage in a header. An
inline function or in-class member that odr-uses one (binds it to a `const&`, takes its address,
passes it to `std::max`/`std::min`, constructs a `std::string` from `kEventFieldOfTurn` inside the
header) would then be defined differently in each TU, which is an ODR violation with no diagnostic.
Today it does not happen: the only header uses are `negward ? kNeg : kPos` at `:659` and `:749`, and
`cInt` takes `std::int64_t` by value, so the variables are not odr-used. The only use of
`kEventFieldOfTurn` is in `compiler.cpp:5577`. The risk arrives with the first emitter file. Fix:
`inline constexpr` on all three (the repository already does this for `kTurnEvent` at
`runtime_abi.hpp:134`). It is a one-word edit outside the "mechanical" text, so either make it in E1
and record it as an eighth edit, or make it in the first emitter slice and write it in that slice's
brief. I would do it now.

### m2. Two direct includes are unused; one needed one is only transitive

Removing `#include ".../epistemic_policy.hpp"` and `".../prelude.hpp"` from the header still
compiles under the production flags (`/tmp/p19-codebase/inc/v`, 0 errors each), whereas removing
`foreign_op_handle.hpp` (3 errors) or `cell_views.hpp` (2 errors) fails. `prelude.hpp` is used
(`PreludeField`, header `:948`) and arrives through another header, so keep it;
`epistemic_policy.hpp` is not named by any declaration in the header (the epistemic types it holds
are used only in out-of-class bodies in `compiler.cpp`, which includes the header itself). Drop it
from the header if the goal is that emitter files do not pay for what they do not use; the cost is
small, so this is not urgent. Also `<ranges>` stays only in `compiler.cpp`, which is right.

### m3. Proof gaps (small)

The harness does not dump `FrameBody` (returned by `takeFrameBodies()`, `compiler.hpp:67`) or the
`CompiledContainer` members (`runtime_abi.hpp:842-851`: `owner`, `cell`, `member`, `members`,
`instances`, labels, per-container `fieldDefaults`); only the sizes of `skills` and `behaviors` are
printed. It also always builds `Gen` with the default `beliefEventDepth`, `conditionFuel` and
`maintainBackoffMs`. For a pure move these cannot change, and the arena dump would expose any change
of emission order that moved those refs, so I do not rate it above MINOR. Adding the missing fields
to `irdump.cpp` would make the harness reusable as the E2+ regression oracle, which is what the plan
needs it for. The Doxygen coverage gate (`tools/check-doxygen-coverage.py`, scans `apps`) now sees
the types as named-namespace symbols in a new file; I did not run it (not checked).

### m4. What the first emitter file needs (not a defect of E1)

`Gen`'s data members and almost all member functions are under `private:` (`:387`), with `run()`,
`takeFrameBodies()` and the constructor public (`:371-386`). An emitter in another file that takes
`Gen &` cannot call the primitives (`cInt`, `cStr`, `movC`, `addSlot`...) or touch `core_`,
`arena_`, `dims_` until they are public (or the emitter is a friend). The handoff and plan already
record this; the slice that adds the first emitter must make that edit and extend the proof harness,
since it will change linkage-visible text. Out-of-class `Gen::` definitions placed in a new `.cpp`
must sit in `namespace ...::codegen` and, as the file-local `static` helpers (`isCompoundStmt`,
`hasDynamicCall`) are not in the header, any helper that a second file also needs must move to the
header as `inline`.

## Questions for the owner

None.

Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

Verdict: PASS
