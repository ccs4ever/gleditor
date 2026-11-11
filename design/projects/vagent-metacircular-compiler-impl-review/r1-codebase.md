# Round 1 Critique — C++23 Codebase Expert (`codebase`)

Audit of `design/vagent-metacircular-compiler-implementation.md` focusing on:

1. Accuracy of every `file:line` citation, function signature, and type in the C++23 codebase.
1. C++23 implementation hazards, missing call sites, overload collisions, and TU ownership
   collisions across Wave 1 and Wave 2 parallel tracks.
1. Code contract conformance for `reify`, `validate`, `registerHead`, `drainPendingRegisters`,
   `runGenesis`, and `EngineP16Ceremony`.

______________________________________________________________________

## Findings

### [CODEBASE-1] BLOCKING: `kMetaDimensionNames` drops 6 apparatus dims and breaks vocabulary

- **Location**: `design/vagent-metacircular-compiler-implementation.md:245-254`, `764` (P5);
  `apps/common/xanadu/vagent/lineage.hpp:58-62`; `apps/common/xanadu/vagent/genesis.cpp:157-186`

- **Evidence**: Line 245–254 states:

  > "Extend `kMetaDimensionNames` (`apps/common/xanadu/vagent/lineage.hpp:58-62`) from 6 entries to
  > 8 entries by filing `"d.lineage"` (`P5`) and `"d.kind-registered"` (`P4`):
  >
  > ````cpp
  > inline constexpr std::array<std::string_view, 8> kMetaDimensionNames{
  >     "d.code",   "d.refs",    "d.ref",          "d.ref.for",
  >     "d.patch",  "d.patch.head", "d.lineage",  "d.kind-registered",
  > };
  > ```"
  > ````

  In the actual codebase (`apps/common/xanadu/vagent/lineage.hpp:58-62`), `kMetaDimensionNames`
  already contains **12** apparatus dimensions:

  ```cpp
  inline constexpr std::string_view kMetaDimensionNames[] = {
      "d.code",  "d.patch",      "d.weight",   "d.synapse",
      "d.entry", "d.patch-head", "d.validate", "d.refs",
      "d.ref",   "d.recipe",     "d.refFor",   "d.mesh.relocation",
  };
  ```

  The snippet in the design document:

  1. Erroneously claims `kMetaDimensionNames` has 6 entries instead of 12.
  1. Drops 6 critical apparatus dimensions: `"d.weight"`, `"d.synapse"`, `"d.entry"`,
     `"d.validate"`, `"d.recipe"`, and `"d.mesh.relocation"`.
  1. Misspells `"d.patch-head"` as `"d.patch.head"` and `"d.refFor"` as `"d.ref.for"`.
  1. Misidentifies the location of `sweepDimensionVocabulary`: lines 253–254 cite
     `lineage.cpp:95-120` (which is `restateScopeRefs`), whereas `sweepDimensionVocabulary` is
     defined in `apps/common/xanadu/vagent/genesis.cpp:164-186`.

  Dropping these dimensions breaks `dimensionVocabulary` in `genesis.cpp:157-163`, causing genesis
  step (b) to fail to resolve apparatus dimensions during incarnation startup.

- **Fix**: Update `kMetaDimensionNames` to extend from 12 entries to 14 entries by preserving all 12
  existing entries intact with their exact names and appending `"d.lineage"` and
  `"d.kind-registered"`:

  ```cpp
  inline constexpr std::array<std::string_view, 14> kMetaDimensionNames{
      "d.code",   "d.patch",          "d.weight",   "d.synapse",
      "d.entry",  "d.patch-head",     "d.validate", "d.refs",
      "d.ref",    "d.recipe",         "d.refFor",   "d.mesh.relocation",
      "d.lineage", "d.kind-registered",
  };
  ```

  Correct citation for `sweepDimensionVocabulary` to
  `apps/common/xanadu/vagent/genesis.cpp:164-186`.

______________________________________________________________________

### [CODEBASE-2] BLOCKING: Method signature collision on proposed public `registerHead`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:258-265`, `410-453`, `763`
  (P4); `apps/common/xanadu/vagent/meta.hpp:684`; `apps/common/xanadu/vagent/meta.cpp:2206-2220`,
  `2431`, `2460`, `2490`

- **Evidence**: In `meta.hpp:684`, `MetaRuntime` already declares:

  ```cpp
  [[nodiscard]] MetaMutation registerHead(CellRef head) const;
  ```

  returning `MetaMutation` (`std::expected<void, std::string>`), implemented in `meta.cpp:2206` to
  append `head` to `binds_.dims.code`.

  Line 259 of the plan proposes declaring on `MetaRuntime`:

  ```cpp
  std::expected<CellRef, std::string> registerHead(CellRef head);
  ```

  In C++23, member functions cannot be overloaded differing only in return type and `const`
  qualifier.

  If the plan intended to replace the existing `registerHead`, this causes a compilation failure
  across existing call sites in `meta.cpp`: At `meta.cpp:2431` (`learnWhere`) and `meta.cpp:2460`
  (`learnBody`):

  ```cpp
  if (const auto ok = registerHead(copies->registered); !ok) return ok;
  ```

  Because `learnWhere` and `learnBody` return `MetaMutation` (`std::expected<void, std::string>`),
  `return ok;` fails to compile if `ok` is `std::expected<CellRef, std::string>` (cannot implicitly
  construct `std::expected<void, E>` from `std::expected<T, E>`).

  Furthermore, `design/vagent-metacircular-compiler.md:282` explicitly designated this primitive as
  `register` *beside* `registerHead` ("one `MetaRuntime` verb (beside `registerHead`, where
  `+d.entry`/`d.code` knowledge lives)").

- **Fix**: Do not collide with or replace private `registerHead`. Name the new public method on
  `MetaRuntime` according to its language verb:

  ```cpp
  [[nodiscard]] std::expected<CellRef, std::string> registerCode(CellRef head);
  ```

  (or `registerFragment(CellRef head)`), accompanied by
  `registerAll(std::span<const CellRef> heads)`. Keep the existing private `registerHead` (or rename
  it `appendCodeRank` / `registerOnCodeRank`) for internal `d.code` rank appends without breaking
  `learn_*` callers.

______________________________________________________________________

### [CODEBASE-3] BLOCKING: `boot:writer` / `boot:linker` violates arity and tail contract

- **Location**: `design/vagent-metacircular-compiler-implementation.md:544-554`, `620-621`, `766`
  (P7); `apps/common/xanadu/vagent/meta.cpp:123`, `587-591`;
  `apps/common/xanadu/vagent/compiler.cpp:1211-1221`, `1889`, `2006-2018`

- **Evidence**: Lines 544–554 state:

  > "`boot:writer(target: Cell, source: Cell): Cell` ... followed by a registered
  > `OpcodeKind::Value` op (`vortex_vm.cpp:1049-1071`: left-wing `[targetSlot, sourceSlot]`,
  > right-wing `[outSlot]`), followed by `RunResume`."
  > "`boot:linker(from: Cell, dim: Cell, dir: Int, to: Cell): Cell` ... followed by `RunResume`."

  And lines 620–621 state:

  > "- **Epilogue (`compiler.cpp:1985-2015`):** Link final expression slot to descriptor's 'result'
  > `d.vars` record (`Value` op), set 'produced' to '1', and emit `RunResume` with
  > `[cursorSlot, defaultFailSlot]`."

  This violates three fundamental engine invariants:

  1. **`Value` wing arity:** In `meta.cpp:123` (`kArity`), `OpcodeKind::Value` requires 4 left-wing
     inputs: `[target, offset, len, replacement]`, matching `compiler.cpp:1211-1221`:
     `bindIn(op, target); bindIn(op, zero); bindIn(op, minusOne); bindIn(op, source);`. Emitting
     `Value` with 2 inputs `[targetSlot, sourceSlot]` fails `MetaRuntime::validate` at
     `meta.cpp:563-565` (`"validate: wing arity differs from the table"`).
  1. **Fragment tail contract:** In `compiler.cpp:1889`, fragment bodies in VAgent ALWAYS tail
     `OpcodeKind::Return` (`emitWrapped(..., [&] { return mint(OpcodeKind::Return); }, ...)`).
     `MetaRuntime::validate` (`meta.cpp:587-591`) strictly enforces that run-reachable op chains
     must tail `Return`. `RunResume` is the *caller's* frame successor following `#RUN`
     (`compiler.cpp:2006-2018`, `vortex_vm.cpp:1097-1145`), NOT a fragment epilogue! If
     `boot:writer` tails `RunResume`, `validate` refuses it as invalid, and running it faults the
     VM.
  1. **Erroneous citations:** `vortex_vm.cpp:1049-1071` and `1072-1091` cited for `Value` and `Link`
     are actually inside `OpcodeKind::RunCall`'s frame setup and argument binding.
     `OpcodeKind::Link` is implemented at `vortex_vm.cpp:774-811`, and `OpcodeKind::Value` is
     implemented at `vortex_vm.cpp:1200-1228`.

- **Fix**:

  1. Specify `boot:writer` with 4 left-wing inputs: `[target, zero, minusOne, source]` and 1
     temporary output.
  1. Require both `boot:writer` and `boot:linker` (and Stage 5's in-language emitter) to follow the
     standard `emitWrapped` structure: parameter `Resolve` prologue, `ChoiceDepth` + `Choice`
     failure wrapper, body op, `Cut`, and tail `Return`. Correct epilogue description in Stage 5
     accordingly.
  1. Correct `vortex_vm.cpp` citations for `Link` (`:774-811`) and `Value` (`:1200-1228`).

______________________________________________________________________

### [CODEBASE-4] MAJOR: `VortexVM::programOpcodes_` and `registerOpcode` do not exist

- **Location**: `design/vagent-metacircular-compiler-implementation.md:19`, `147`, `404`, `425`,
  `430`, `447`, `465`, `489`, `763` (P4); `apps/common/xanadu/vortex/vortex_vm.hpp:154, 269`,
  `apps/common/xanadu/vortex/vortex_vm.cpp:186`

- **Evidence**: Line 19 asserts:

  > "Because Vortex `>Value` and `>Link` instructions cannot set the out-of-band
  > `VortexVM::programOpcodes_` table (`apps/common/xanadu/vortex/vortex_vm.hpp:239`),"

  and lines 404, 425 assert calling `vm_.registerOpcode(op, kind)`.

  In the real C++ code:

  1. Line 239 of `vortex_vm.hpp` is `ExecutionResult step(CellRef cursor);`.
  1. The member variable on `VortexVM` is `std::unordered_map<CellRef, OpcodeKind> opcodeMap_;`
     (`vortex_vm.hpp:269`). The symbol `programOpcodes_` does not exist anywhere in the repository.
  1. The method to set an opcode kind in `opcodeMap_` is
     `void VortexVM::setOpcodeKind(CellRef opCell, OpcodeKind op);` (`vortex_vm.hpp:154`,
     `vortex_vm.cpp:186`). There is no method named `registerOpcode`.

- **Fix**: Replace `programOpcodes_` with `opcodeMap_` throughout the document. Update registration
  calls to `vm_.setOpcodeKind(op, kind)` (or declare
  `void registerOpcode(CellRef op, OpcodeKind kind) noexcept { setOpcodeKind(op, kind); }` on
  `VortexVM` as a designated synonym).

______________________________________________________________________

### [CODEBASE-5] MAJOR: TU ownership and scheduling conflict between Track B and Track C

- **Location**: `design/vagent-metacircular-compiler-implementation.md:209-222`, `438-439`, `763`
  (P4); `apps/common/xanadu/vagent/executor.cpp:3024-3030`

- **Evidence**: Section 2 File-Ownership Matrix assigns `executor.cpp` exclusively to Track C
  ("Stage 1 std:meta Lowering & runHostStep Dispatch: compiler.cpp, decls.cpp, executor.cpp"). Track
  B is assigned exclusively `meta.cpp` and test files.

  However, Stage 2 P4 requires invoking `drainPendingRegisters()` in `VagentExecutor::executeTurn`
  at `executor.cpp:3029` (immediately after `vm.cut(0); arena.discard(m0);`).

  In the proposal register table (line 763), P4 lists `executor.cpp:3024-3030` under target files.

  If Track B and Track C work in parallel worktrees concurrently:

  - If Track B edits `executor.cpp:3029`, it violates the "Zero TU Collisions" contract and creates
    git merge conflicts with Track C's extensive edits to `runHostStep` in `executor.cpp:808-1642`.
  - If Track B does not touch `executor.cpp`, Track B's tests cannot verify turn-boundary
    registration draining in `executeTurn`.

- **Fix**: Explicitly designate `executor.cpp` ownership: Track C owns all modifications to
  `executor.cpp`, including wiring the call to `meta.drainPendingRegisters()` at line 3029. Track B
  implements `MetaRuntime::drainPendingRegisters()` in `meta.cpp` and tests it directly via
  kernel-level tests (`vagent_meta_register_test.cpp`). Turn-level integration through `executeTurn`
  is verified at Barrier 1 after Tracks B and C merge.

______________________________________________________________________

### [CODEBASE-6] MAJOR: Imaginary `ArityRule` type and wrong citation for `Linked` in `meta.cpp`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:99`, `375-378`, `760` (P1);
  `apps/common/xanadu/vagent/meta.cpp:102-158`, `vortex_vm.cpp:812-870`

- **Evidence**: Line 375–378 states:

  > "`Linked` Arity in `kArity` (`meta.cpp:121`): Update `OpcodeKind::Linked`'s `ArityRule` from
  > `{4, 4, 1, 1}` to allow 4 or 6 left-wing inputs (`minLeft = 4, maxLeft = 6` — extending
  > `ArityRule` with `minLeft`/`maxLeft` or an explicit
  > `op == OpcodeKind::Linked && (left == 4 || left == 6)` check at `meta.cpp:563`), matching
  > `vortex_vm.cpp:950-1009`."

  In `meta.cpp:102-158`:

  1. `kArity` is an array of `(op, in, out)` triples:
     `static constexpr std::int64_t kArity[][3] = { ... };`. There is no `ArityRule` struct and no
     `{4, 4, 1, 1}` quadruple.
  1. Line 121 is simply `{static_cast<std::int64_t>(OpcodeKind::Linked), 4, 1},`.
  1. `vortex_vm.cpp:950-1009` is `OpcodeKind::RunCall`. `OpcodeKind::Linked` is implemented at
     `vortex_vm.cpp:812-870`, where inputs can be 3, 4, or 6 (`inCells.size() >= 3`, optional 4th
     `follow`, optional 5th and 6th `filterDim`/`filterNeg`).

- **Fix**: Correct the description: `kArity` stores `std::int64_t[3]` triples. Because `Linked`
  accepts 3, 4, or 6 inputs (`vortex_vm.cpp:812-870`), update `MetaRuntime::validate` at
  `meta.cpp:563` to admit `(inCells.size() == 3 || inCells.size() == 4 || inCells.size() == 6)` when
  `*ordinal == static_cast<std::int64_t>(OpcodeKind::Linked)`. Correct citation to
  `apps/common/xanadu/vortex/vortex_vm.cpp:812-870`.

______________________________________________________________________

### [CODEBASE-7] MAJOR: `RunCall` wing slot 2 is `failOp`, not continuation

- **Location**: `design/vagent-metacircular-compiler-implementation.md:367`, `760` (P1);
  `apps/common/xanadu/vagent/compiler.cpp:2013-2018`, `vortex_vm.cpp:957-964`

- **Evidence**: Line 367 states:

  > "- `RunCall`: read left-wing slot 2 (continuation) → dereference operand cell as `asInt64` →
  > target `CellRef` (`compiler.cpp:1933`)."

  In `compiler.cpp:2013-2018` (`emitRunDynamic`):

  ```cpp
  const auto runCall = mint(OpcodeKind::RunCall);
  bindIn(runCall, box);                                                       // slot 0: head/box
  bindIn(runCall, dimRef(core_.findOrMintDimension("d.entry"), "d.entry"));   // slot 1: entryDim
  bindIn(runCall, reference(cells_->fail));                                  // slot 2: failOp
  spin(runCall, runResume);                                                  // +d.spin: continuation
  ```

  And in `vortex_vm.cpp:957-964`: Input 0 is `head`, Input 1 is `entryDim`, Input 2 is `failOp`,
  Input 3 is `argsRank`. Slot 2 is `failOp` (the default failure jump target), NOT the continuation!
  The continuation op is linked on `+d.spin` (`runResume`).

  Furthermore, `compiler.cpp:1933` is empty space preceding `emitRun` (which is static `#Call`);
  dynamic `RunCall` is `emitRunDynamic` at lines 2004–2040.

- **Fix**: Update two-hop validation for `RunCall`:

  1. Left-wing slot 2 is validated as a jump target (`failOp`) in `opSet` (if non-zero).
  1. The continuation is validated by following `+d.spin` to `RunResume`. Correct the code citation
     to `apps/common/xanadu/vagent/compiler.cpp:2013-2018`.

______________________________________________________________________

### [CODEBASE-8] MINOR: Citations in `meta.cpp` for `validate`, `verifyClosed` are shifted

- **Location**: `design/vagent-metacircular-compiler-implementation.md:21`, `359`, `388-389`, `456`,
  `760`, `762`, `767` (P1, P3, P8); `apps/common/xanadu/vagent/meta.cpp`

- **Evidence**:

  - `MetaRuntime::validate` is cited as `meta.cpp:518-634` (lines 21, 359, 760, 762). In reality,
    `validate` begins at line 490 and ends at line 623. Line 518 is midway through decoding
    `arityTable`. Lines 626–632 belong to `weightOf`.
  - Line 379 claims lines 626–632 contain an `ensureDimension("d.validate")` and `makeCell("ok")`
    loop. In reality, `d.validate` tagging is at lines 530–538 (`arena.makeScalarCell(*serial)`).
  - `MetaRuntime::verifyClosed` is cited as `meta.cpp:748-773` (lines 388, 762). In reality, lines
    748–773 are inside `MetaRuntime::runFragment` (frame setup and parameter binding).
    `verifyClosed` is at lines 2700–2775.
  - `MetaRuntime::programCells` is cited as `meta.cpp:191-215` (lines 389, 762). In reality, lines
    191–215 are `mintMetaBindSet` ending and `moduleNamed`. `programCells` is at lines 2156–2184.
  - `MetaRuntime::learnCopies` is cited as `meta.cpp:1906-1915` (lines 456, 767). In reality, lines
    1906–1915 are `clauseFormals` and `matchAttach`. `learnCopies` is at lines 2189–2204.
  - Line 360 cites `controlTargets` and `wingPositions` in `meta.cpp:588`. These identifiers do not
    exist; lines 597–600 define `const auto target = [&](std::size_t index)`.

- **Fix**: Correct citations in `meta.cpp`: `validate` is 490–623 (tagging at 530–538);
  `verifyClosed` is 2700–2775; `programCells` is 2156–2184; `learnCopies` is 2189–2204. Remove
  references to non-existent identifiers `controlTargets` and `wingPositions`.

______________________________________________________________________

### [CODEBASE-9] MINOR: Citations in `fragment.cpp`, `compiler.cpp`, and `checker.cpp` are misplaced

- **Location**: `design/vagent-metacircular-compiler-implementation.md:273`, `278`, `348`,
  `604-633`; `apps/common/xanadu/vagent/`

- **Evidence**:

  - Line 604 cites Prologue emission as `fragment.cpp:69-108`. In reality, `fragment.cpp:69-108` is
    `struct RecordEmitter`. Parameter resolution prologue is emitted in `compiler.cpp:1890-1895`.
  - Line 622 cites Descriptor emission as `fragment.cpp:110-185`. In reality, descriptor emission is
    in `fragment.cpp:252-318`.
  - Line 628 cites `+d.refs` and `+d.recipe` as `fragment.cpp:190-310`. In reality, coverage set and
    recipe/ref record emission are in `fragment.cpp:320-440`.
  - Line 278 cites `Compiler::compileCall` (`compiler.cpp:975-1180`). In reality, the class is
    `Emitter`, no method `compileCall` exists (lines 975–1180 are `preflight` and helpers), and call
    emission is `Emitter::emit` (`compiler.cpp:1576-1730`).
  - Lines 273 and 348 cite `std:meta` function signatures as `checker.cpp:1049-1231`. In reality,
    lines 1049–1231 are `pureBody`, `derivePurity`, `actionAllowed`, `arguments`, `metaForm`,
    `metaRun`, `metaReify`. The `std:meta` built-in function table is located at
    `apps/common/xanadu/vagent/checker.cpp:966-1021`.

- **Fix**: Update all citations to match actual file structures: `fragment.cpp:252-318`
  (descriptor), `fragment.cpp:320-440` (records/recipes), `compiler.cpp:1890-1895` (prologue),
  `compiler.cpp:1576-1730` (`Emitter::emit`), and `checker.cpp:966-1021` (`std:meta` signatures).

______________________________________________________________________

### [CODEBASE-10] MINOR: Non-existent `noDim` sentinel and wrong struct location in `meta.hpp`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:255-257`;
  `apps/common/xanadu/vagent/meta.hpp:72-85`

- **Evidence**: Line 255–257 states:

  > "Add `DimRef lineage{noDim};` and `DimRef kindRegistered{noDim};` to `MetaDimensions`
  > (`meta.hpp:43-50`)."

  In reality:

  1. Lines 43–50 of `meta.hpp` are `#include` directives. `MetaDimensions` is located at lines
     72–85.
  1. The identifier `noDim` does not exist anywhere in the gleditor/xuzz codebase. `MetaDimensions`
     default-initializes `DimRef` fields with `{noCell}` (`DimRef code{noCell};`,
     `using zigzag::noCell;`).

- **Fix**: Update citation to `meta.hpp:72-85` and use `{noCell}`:

  ```cpp
  DimRef lineage{noCell};
  DimRef kindRegistered{noCell};
  ```

______________________________________________________________________

### [CODEBASE-11] NIT: False assertion that `runFragment` has its own mark to discard

- **Location**: `design/vagent-metacircular-compiler-implementation.md:440-442`;
  `apps/common/xanadu/vagent/meta.cpp:700-825`

- **Evidence**: Line 440–442 specifies as a registration drain boundary:

  > "2. `MetaRuntime::runFragment` / `runBox` when called from an outer caller where
  > `arena.outstandingMarks() == 0 && vm_.scopedDepth() == 0` after its own mark is discarded."

  `MetaRuntime::runFragment` / `runBox` does not open or discard any `zigzag::Mark`. It runs under
  the caller's mark environment and only restores cursor stack/vars ranks upon completion
  (`meta.cpp:786-811`).

- **Fix**: Clarify that `runFragment` does not open or discard marks; post-commit register draining
  is strictly driven by turn boundaries (`VagentExecutor::executeTurn` at line 3029) or explicit
  test harnesses at `outstandingMarks() == 0`.

______________________________________________________________________

SUMMARY: BLOCKING=3 MAJOR=4 MINOR=3 NIT=1 VERDICT: NEEDS-REVISION
