# E1 implementation review, round 1: System architect (persona architect)

Scope: commit `0dc6bbad` (`compiler.cpp` to new `compiler_gen.hpp`), the worker handoff
`/tmp/p19-e1/handoff.md`, the IR-dump harness under `/tmp/p19-e1/harness/`, and the compiler
reviewer's report `review-codebase.md`, against slice E1 and section 4.2 of `remaining-plan.md` and
the I3-2 design's file plan. My angle is the architecture: the header as the seam for new emitter
files, namespace and access choices, and what the first emitter slice must change. Code at HEAD
`d710e7a3` (E1 unchanged since `0dc6bbad`). Scripts and probes: `/tmp/p19-architect/e1/`
(`movecheck.py`, `p/a.cpp`, `p/c.cpp`, `p/d.cpp`).

## What is sound

- **The move is mechanical.** `movecheck.py` compares `compiler.cpp` lines 92-1278 at `3b86b36d`
  (1,187 lines, `namespace {` through the end of `class Gen`) with the body of `codegen` in the
  header, ignoring blank lines and indentation. The only differences are the four `using`
  declarations the header adds and `inline` on `bail`, `unsupported`, `tyOf`, `S`, `D`, `Hd` and
  `collectCallees`. The rest of `compiler.cpp` differs by `} // namespace codegen` and the three
  `codegen::` qualifiers in `compileAgent`; the prefix differs by the include and the namespace
  line. This agrees with the handoff and with the compiler reviewer.
- **Linkage.** A translation unit that includes only the header and defines one function exports one
  symbol (`nm`, `p/a.cpp`): no definition from the header becomes a strong symbol. The header is
  standalone (compiles in 4.3 s at `-O0` with the production include paths and `-Werror`). The
  out-of-class `Gen::` members stay in `compiler.cpp`, defined once. The two file-local helpers stay
  `static` inside `codegen` and keep internal linkage.
- **Coexistence with `checker.hpp`.** `p/c.cpp` includes both headers and compiles. Inside
  `codegen`, unqualified `FieldInfo` is `codegen::FieldInfo`; in `runtime` scope it is the
  checker's. Nothing is ambiguous, because `codegen` is a nested namespace and no `using namespace`
  exists. See m3 for the one trap.
- **Namespace.** A named `codegen` namespace is the right choice for a header: an anonymous one
  would give every including file its own `Gen`. Nothing else in `apps src include tests tools`
  declares `codegen` (grep). The name is accurate: it holds the generator, not the compiler front
  end (`Compiler` stays in `runtime`).
- **Header hygiene.** Include guard in the repository spelling, a file comment that says why the
  header exists and who may include it, `using` declarations inside `codegen`, no `static` at
  namespace scope except the three constants of m1.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- **m1. Three namespace-scope constants have internal linkage in a header.** `kPos`, `kNeg` and
  `kEventFieldOfTurn` (`compiler_gen.hpp:93-95`) are `constexpr`, not `inline constexpr`. Today no
  inline function odr-uses them: `negward ? kNeg : kPos` (`:659`, `:749`) is a potential result and
  `cInt` takes its argument by value. The first emitter that binds one to a reference, or passes it
  to `std::max`, creates an ODR violation that no diagnostic reports. I agree with the compiler
  reviewer: make it `inline constexpr` (as `kTurnEvent` is at `runtime_abi.hpp:134`) before the
  first emitter file lands. It is a one-word edit in three places, so make it a small commit of its
  own and rerun the IR dump.

- **m2. E1 left Gen's access unchanged, and that is the right call; the plan text is stale.** The
  plan's E1 entry says "the members that emitters use become public" and its standing rules say "a
  new emitter is a free function taking `Gen&` in its own file, so the private header stops
  changing" (`remaining-plan.md:93`, E1 content). The handoff records that this was not done. Four
  facts decide it:

  1. `Gen` has 850 lines of private declarations (`compiler_gen.hpp:387-1236`): about 200 members
     and all the state. A free function needs most of them (`cInt`, `movC`, `mkOp`, `core_`,
     `arena_`, `dims_`), so "the ones emitters use" means "all of them", which turns an
     implementation class into a public interface of a private header. A `friend` declaration per
     emitter does the same job with more text.
  1. The lazy-routine mechanism already stores `void (Gen::*)()` (`compiler_gen.hpp:416-423`,
     `routineLabel`, used at `compiler.cpp:248`, `275`, `306`, `312`, `317`). A free function cannot
     be queued without changing that type.
  1. The I3-2 design already assumes members: "out-of-class `Gen` members in new files", "a
     declaration that a new file needs from `Gen` goes in `compiler_gen.hpp`" (sections 1, 3.8,
     4.3), and the plan's own record at `remaining-plan.md:1799` accepts "a later header edit of a
     few lines per area".
  1. An out-of-class member defined in `namespace xanadu::vagent::runtime::codegen` needs no access
     change at all.

  So the header does change, by declarations, once per slice. Amend the plan: replace "emitters are
  free functions" with "an emitter is an out-of-class `Gen` member defined in
  `epistemic_emit_<area>.cpp`, declared in a labelled section of `compiler_gen.hpp`", and change the
  `Code generator seam` row from "E1 only" to the slices that add declarations. State the cost: the
  header costs 4.3 s to parse and every declaration edit rebuilds `compiler.cpp` and each emitter
  file, so one slice makes one hunk per area.

- **m3. Unqualified names inside `codegen` shadow the checker's types without a diagnostic.**
  `codegen` redeclares `FieldInfo`, `MethodInfo`, `GoalInfo`, `PlanInfo` and `QuestionInfo`, which
  `checker.hpp` declares in `runtime` (`:135`, `:185`, `:205`, `:212`, `:178`). `p/c.cpp`
  static-asserts that an emitter-style scope inside `codegen` gets the generator's type even when
  `checker.hpp` is included. Emitters consume the AST (`Unit` from `ast.hpp`), not the checker's
  tables, so no emitter should include `checker.hpp`. Add one sentence to the header's file comment
  saying so and that `runtime::FieldInfo` must be spelled out if both are ever needed. Renaming the
  generator's types is not worth the churn.

- **m4. Keep `epistemic_policy.hpp`; the compiler reviewer's suggestion to drop it is wrong.**
  `mintPolicyMaster(CellRef, const ArbitrationPolicy &)` (`compiler_gen.hpp:1173`) names the type.
  With the include deleted a standalone translation unit fails:
  `unknown type name 'ArbitrationPolicy'` (`p/d.cpp`). The include is needed and the "emitters pay
  for what they do not use" cost is the price of the declaration. (`prelude.hpp` and the other
  includes are used.)

- **m5. The proof is enough for the move and is not yet an oracle for the next slices.** The dump
  records, for each of 41 compiled agents, every cell's text, value kind and bits, flags, birth and
  last op, every link and every entry ref, plus 27 refusals (304,449 lines, byte-equal; checked
  `sha256` of `base.out` and `new.out`, both `d443ebb1...`). Anything an emitter edit could change
  in the IR shows. Gaps: `FrameBody` and the `CompiledContainer` fields are not dumped (compiler
  review m3), and the harness is throwaway under `/tmp/p19-e1`. For the slices that follow, equality
  cannot hold for agents that gain a health routine, so the dump is useful in two other forms: (a)
  as a byte-equality check for agents with no arbitrated predicate, which requires that a new
  routine's `newLabel()` is minted only when `observeEntry` is (`newLabel()` mints a `Nop` op cell,
  `compiler_gen.hpp:598`, so one unconditional label in the `run()` block at
  `compiler.cpp:6279-6283` renumbers every `CellRef` of every agent); and (b) as a structural diff
  for arbitrating agents that lists only added cells. Save the harness somewhere durable (it is not
  a repository file by decision; a copy next to the project's validation record is enough) and add
  the two missing structures before 2a.

- **m6. A first-slice checklist is not written anywhere.** See the next section.

## What the first emitter slice must change

For I3-2a (`epistemic_emit_health.cpp`, `rtHealthUpdate_`, `rtHealth_`), none of this needs another
E1 commit:

1. **`compiler_gen.hpp`:** the m1 `inline constexpr` fix; one labelled section "health
   (`epistemic_emit_health.cpp`)" next to the belief labels (`:1142`) holding the new label members
   and the `void emitRt...()` declarations. No access change, no new include other than what the new
   declarations name.
1. **`compiler.cpp` `run()`:** mint the new labels and call the new emitters only when an arbitrated
   predicate exists (as `observeEntry` is, `:6351`), add the new `CompiledAgent` entries and the
   existing-routine edits the design lists (`kIngestOrder`, the ingestion routine's read list). Stay
   within the plan's 400 lines for a file over 1,500.
1. **The new file:** include `compiler_gen.hpp` and nothing from `checker.hpp`; define `Gen::`
   members inside `namespace xanadu::vagent::runtime::codegen`. Any helper that both `compiler.cpp`
   and the new file need goes into the header as `inline`; the `static` helpers `isCompoundStmt`
   (`:2136`) and `hasDynamicCall` (`:5191`) and the function-local `kIngestOrder` (`:4079`) are not
   visible to another file. Constants the IR and the host share belong in
   `epistemic_abi_health.hpp`.
1. **Build and test wiring:** none. The Makefile finds sources (`Makefile:636-642`) and the actor
   test takes `tests/xuzz/vagent_%` (`:1170`); `vagent_runtime_test` removes every `vagent/` object
   (`:694`), so tests that link the new file go in `vagent_actor_test`.
1. **Proof:** run the (saved, extended) dump on the corpus before and after: non-arbitrating agents
   byte-equal; arbitrating agents differ only by added cells and the new entry refs.

Tally: 0 BLOCKING, 0 MAJOR, 6 MINOR.

Verdict: PASS
