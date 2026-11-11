# Round 1 critique: Codebase Expert (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 4 BLOCKING, 4 MAJOR, 5 MINOR.

## BLOCKING

### 1. (§2.2, line 149 & §7, line 499) Hallucinated `consumeWork(file_byte_size)` method signature

Full title: Hallucinated parameterised `consumeWork(file_byte_size)` method on `ArenaManifold`

**Problem:** The design asserts that `std:io:read_file` prevents memory exhaustion by calling
`WorkerContext::arena.consumeWork(file_byte_size)` and cites bounds-checked work consumption in
Proposal P26. However, `ArenaManifold::consumeWork()` takes zero arguments and decrements evaluation
cycle fuel, not byte allocations. Attempting to call `consumeWork(file_byte_size)` is a compilation
error in C++23.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:149`:

  > `- Work consumption: WorkerContext::arena.consumeWork(file_byte_size)`
  > `prevents memory exhaustion.`

- In `design/vagent-metacircular-missing-features.md:499` (P26):

  > `bounds-checked work consumption via consumeWork.`

- In `apps/common/xanadu/zigzag/arena_manifold.hpp:486–492`:

  ```cpp
  void consumeWork() const {
    if (workRemaining_) {
      if (*workRemaining_ == 0)
        throw std::runtime_error("Vortex evaluation work budget exhausted");
      --*workRemaining_;
    }
  }
  ```

  `ArenaManifold::consumeWork()` is parameterless. Scratch byte allocation limits are tracked
  separately via `scratchLimit_` and configured via
  `ArenaManifold::setExecutionLimits(cells, work, scratchBytes)` (`arena_manifold.hpp:475–482`).

**Fix:** Update §2.2 and Proposal P26 to reflect actual `ArenaManifold` resource bounds. The host
shim must check `file_byte_size` against `scratchLimit_` or `ActorGrant::maxOutputBytes`, and call
`arena.consumeWork()` once per read invocation (or in a loop per chunk if incremental), while
charging byte allocation against the scratch buffer budget.

### 2. (§2.2, line 151) Hallucinated `ActorError::GrantExceeded` enum variant

Full title: Hallucinated `ActorError::GrantExceeded` error code for file output cap refusal

**Problem:** The design states that files exceeding `ActorGrant::maxOutputBytes` are refused with
`ActorError::GrantExceeded`. This enum constant does not exist in `ActorError`.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:150–152`:

  > `- Buffer cap: Files exceeding ActorGrant::maxOutputBytes (default 32 KiB, configurable per`
  > `grant) are refused with ActorError::GrantExceeded.`

- In `apps/common/xanadu/vortex/mesh_actor.hpp:39–56`:

  ```cpp
  enum class ActorError {
    NotAuthorized,
    InvalidMessage,
    UnknownProcess,
    WrongEpoch,
    MailboxFull,
    ResourceLimit,
    DuplicateMismatch,
    NotImmutable,
    ExecutionFailed,
    TransportUnavailable,
    NotQuiescent,
    Refused,
  };
  ```

  There is no `GrantExceeded` enumerator. The standard error code for resource ceiling violations is
  `ActorError::ResourceLimit`.

**Fix:** Replace `ActorError::GrantExceeded` with `ActorError::ResourceLimit` across §2.2 and any
corresponding error-handling specifications.

### 3. (§3.2, lines 209-210) Contradiction of `ValueKind` and `CompactOpNode` invariants

Full title: Inclusion of `String` as a scalar `ValueKind` in `std:zigzag:set_value`

**Problem:** §3.2 states that `set_value(cell, value)` updates the scalar value and value kind
"(`Int64`, `Double`, `String`, `None`)" on an existing cell by calling
`VortexCore::arena().setValueBits(...)`. In the Nelsonian hyperstructural storage model, `String` is
not a scalar `ValueKind`. Text content is primedia stored in the scratch buffer or permascroll,
never bit-packed into scalar bits.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:209–210`:

  > `3. set_value(cell: CellRef, value: any) -> CellRef: Updates the scalar value and value kind`
  > `(Int64, Double, String, None) on an existing cell (VortexCore::arena().setValueBits(...)).`

- In `apps/common/xanadu/ops.hpp:154–163`:

  ```cpp
  enum class ValueKind : std::uint8_t {
    None            = 0,
    Double          = 1,
    Bool            = 2,
    Int64           = 3,
    OpHandle        = 4,
    ExternRef       = 5,
    Timestamp       = 6,
    ForeignOpHandle = 7,
  };
  ```

- In `apps/common/xanadu/compact_op.hpp:64`: `CompactOpNode::value` holds canonical scalar bits for
  Double, Bool, Int64, OpHandle, ExternRef, Timestamp, and ForeignOpHandle. Textual string content
  is addressed via `span` (`ScrollId`, `spanStart`, `spanLength`), not `value`.

- In `apps/common/xanadu/zigzag/arena_manifold.hpp:670–671`: `setValueBits` sets `ValueKind` and
  `uint64_t bits`. Content text is updated separately via `setContent`
  (`arena_manifold.hpp:666–667`).

**Fix:** Correct §3.2: `set_value` sets scalar value kinds (`Int64`, `Double`, `Bool`, `Timestamp`,
`OpHandle`, `None`). If a cell's string text needs to be replaced, provide an explicit
`set_text(cell: CellRef, text: string) -> CellRef` primitive that allocates an interned scratch span
and updates the cell's content, maintaining the separation between primedia text and scalar bits.

### 4. (§3.2, lines 200, 215-218 & §7, line 500) `std:zigzag:promote` signature and store mutability

Full title: `std:zigzag:promote` lacks store mutability, failure handling, and parameter context

**Problem:** §3.2 proposes `promote(root: CellRef) -> string` which calls
`zigzag::promote(store, parentVersion, arena, root)` (`arena_manifold.hpp:930–934`). This
specification contains three critical defects:

1. `zigzag::promote` requires a mutable `xanadu::Store &store`. In `VortexStdLib`, the member is
   `const xanadu::Store *boundStore_{nullptr};` (`vortex_stdlib.hpp:505`). A const store cannot be
   passed to `promote`. P27 does not propose changing `boundStore_` to mutable `xanadu::Store *`.
1. `promote` returns `std::optional<Promoted>`, where `Promoted` carries the version and cell
   mappings (`arena_manifold.hpp:303–307`). When promotion fails (e.g. root unreached, budget
   exceeded), it returns `std::nullopt`. Exposing `promote(root) -> string` ignores failure and
   forces an empty string sentinel, violating the house style rule "No Sentinels in APIs".
1. The primitive does not specify where `parentVersion` is obtained when called from within VAgent
   (e.g., `store->primaryCurrentVersion()`), nor what happens if `boundStore_` is null.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:200, 215–218`:

  > `+promote(root) string`
  > `promote(root: CellRef) -> string: Executes zigzag::promote(store, parentVersion, arena, root)`

- In `apps/common/xanadu/zigzag/arena_manifold.hpp:930–934`:

  ```cpp
  [[nodiscard]] std::optional<Promoted>
  promote(xanadu::Store &store, const xanadu::MicroversionId &parent,
          const ArenaManifold &from, CellRef root, PromotionBudget budget = {},
          std::span<const CellRef> extraRoots = {});
  ```

- In `apps/common/xanadu/vortex/vortex_stdlib.hpp:505`: `const xanadu::Store *boundStore_{nullptr};`

**Fix:**

1. Update `std:zigzag:promote` signature in VAgent to return `Result[string, string]` or
   `Option[string]` to properly report failure without sentinels.
1. In Proposal P27, explicitly amend `apps/common/xanadu/vortex/vortex_stdlib.hpp` to change
   `boundStore_` to mutable `xanadu::Store *` and add `setBoundStore(xanadu::Store *)`.
1. Specify that `promote` obtains `parentVersion` from `boundStore_->primaryCurrentVersion()`,
   passes the actor's remaining allocation quota as `PromotionBudget`, and returns
   `Err("StoreUnbound")` if `boundStore_` is nullptr.

## MAJOR

### 1. (§7, line 503) Incomplete file inventory in Proposal P30 for AST and R1 vocabulary additions

Full title: Proposal P30 omits `ast.hpp`, `r1_ast.hpp/.cpp`, and stdlib compiler modules

**Problem:** Proposal P30 scopes `match` expressions and tuple destructuring syntax modifications to
`parser.cpp`, `checker.cpp`, `compiler.cpp`, and `stdlib/parser.vagent`. However, introducing
`match` and tuple patterns alters the core AST data structures and the R1 AST manifold serialization
format. These modifications require changes to `ast.hpp`, `r1_ast.hpp`, `r1_ast.cpp`, and sibling
stdlib modules that are missing from P30.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:503`:

  > `P30 | apps/common/xanadu/vagent/parser.cpp, checker.cpp, compiler.cpp, stdlib/parser.vagent`

- In `apps/common/xanadu/vagent/ast.hpp:40–92`: `enum class ExprKind` defines expression node kinds
  (`If`, `Let`, `Block`, etc.). It lacks `Match`, `MatchArm`, and pattern kinds.

- In `apps/common/xanadu/vagent/r1_ast.hpp:13–20`: `kR1AstKinds` defines the canonical 27 R1 AST
  kinds filed on `+d.ast.kinds`. It lacks `"match"`, `"match.arm"`, and pattern descriptors.

- In `apps/common/xanadu/vagent/r1_ast.cpp`: `marshalAstToR1` and `unmarshalR1ToAst` must be
  extended to serialize match nodes across `d.grab`, `d.spin`, and `d.step` ranks.

- In `apps/common/xanadu/vagent/stdlib/ast.vagent`, `checker.vagent`, `emitter.vagent`: The
  metacircular compiler passes must be updated to classify, typecheck, and lower the new AST nodes.

**Fix:** Expand the file list of Proposal P30 to include:

- `apps/common/xanadu/vagent/ast.hpp`
- `apps/common/xanadu/vagent/r1_ast.hpp` and `r1_ast.cpp`
- `apps/common/xanadu/vagent/stdlib/ast.vagent`
- `apps/common/xanadu/vagent/stdlib/checker.vagent`
- `apps/common/xanadu/vagent/stdlib/emitter.vagent`

### 2. (§5.1-5.2, §7, line 502) Layer boundary violation in Proposal P29 (`std:actor:spawn_image`)

Full title: Proposal P29 places `MeshActorRuntime` dispatch inside generic `VortexStdLib`

**Problem:** Proposal P29 specifies adding `std:actor:spawn_image` to
`apps/common/xanadu/vortex/vortex_stdlib.cpp` dispatching through `MeshActorRuntime::spawn`. This
violates architectural layering: `VortexStdLib` is generic runtime infrastructure that has no
knowledge of `MeshActorRuntime`, `VagentAdapter`, `Store`, or actor lineages. In the live
architecture, `MeshActorRuntime` owns `WorkerContext`, which owns `VortexStdLib`. Coupling
`VortexStdLib` directly to `MeshActorRuntime` introduces an inverted circular dependency.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:502`:

  > `P29 | apps/common/xanadu/vortex/vortex_stdlib.cpp, apps/common/xanadu/vagent/adapter.cpp |`
  > `Expose std:actor:spawn_image(root, name, grant) dispatching through MeshActorRuntime::spawn`
  > `with ActorGenesis.`

- `apps/common/xanadu/vortex/vortex_stdlib.hpp` contains zero references to `MeshActorRuntime`.

- In `apps/vagent/main.cpp:275–296`, agent spawns are dispatched via a host callback installed on
  `VagentAdapter`:

  ```cpp
  adapter.setSpawnSender([&](WorkerContext &ctx, CellRef member, std::string_view reqId) {
    ...
    return runtime.spawn(identity, request, wallNow, ActorLineage{fold.get(), &lineage});
  });
  ```

- `MeshActorRuntime::spawn` (`mesh_actor.hpp:218–222`) takes
  `(identity, request, wallNow, lineage)`. It does not accept an in-arena `image_root: CellRef`. As
  detailed in §5.2, turning `image_root` into a live actor requires `MetaRuntime::validate`,
  `Store::rebuildManifold`, and `ActorLineage`.

**Fix:** Clarify the dispatch seam for `spawn_image`. Rather than hardcoding `MeshActorRuntime`
calls inside `vortex_stdlib.cpp`, expose `spawn_image` as a host capability hook in `VortexVM` /
`VagentAdapter`. The host runner (`apps/vagent/main.cpp`) or `VagentAdapter` provides the callback
that performs validation, promotion, manifold rebuilding, lineage construction, and runtime
spawning.

### 3. (§2.2, §7, line 499) Missing host argument injection mechanism in Proposal P26

Full title: Proposal P26 lacks host CLI argument injection mechanism for `std:env:args`

**Problem:** §2.2 introduces `std:env:args() -> list[string]`. However, neither `VortexCore`,
`WorkerContext`, nor `VortexStdLib` currently stores or receives process invocation arguments. In
`apps/vagent/main.cpp`, arguments are parsed locally and discarded. For `std:env:args()` to
function, P26 must specify the API on `VortexStdLib` or `WorkerContext` used by `main.cpp` to
forward CLI arguments into the execution context.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:137, 499`:

  > `def args() -> list[string]`

- In `apps/vagent/main.cpp:337–426`: `main(int argc, char **argv)` parses options (`--send`,
  `--node`, etc.) but provides no injection call into `WorkerContext` or `VagentAdapter`.

- `apps/common/xanadu/vortex/vortex_stdlib.hpp` has no `setArgs` or `args_` member.

**Fix:** Update Proposal P26 to include `VortexStdLib::setArgs(std::vector<std::string>)` in
`vortex_stdlib.hpp`, and update `apps/vagent/main.cpp` to forward unconsumed positional CLI
arguments into the primary `WorkerContext` before invoking `runFile`.

### 4. (§2.1-2.2, §7, line 499) Missing citation of existing host file ingestion in `main.cpp:56-76`

Full title: Proposal P26 omits citation of existing host file-reading implementation in `main.cpp`

**Problem:** §2.1 and §2.2 discuss the need for epistemic ingestion (`std:io:read_file` and
`std:io:read_stdin`) and list `apps/vagent/main.cpp` in Proposal P26, but fail to cite the existing
implementation in `main.cpp:56–76`. This violates the standing rule "Argued, Not Asserted", which
requires citing existing code that is being refactored or superseded.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:499`: `apps/vagent/main.cpp` is cited without
  line numbers.

- In `apps/vagent/main.cpp:56–76`:

  ```cpp
  bool readSource(std::string_view path, std::string &source) {
    if (path == "-") {
      source.assign(std::istreambuf_iterator<char>(std::cin), {});
      ...
    }
    std::ifstream input{std::string(path)};
    ...
  }
  ```

  `readSource` currently implements the exact host file and stdin reading logic that `std:io`
  replaces.

**Fix:** Add citation `apps/vagent/main.cpp:56–76` (`readSource`) to §2.1, §2.2, and Proposal P26,
noting that `std:io:read_file` and `std:io:read_stdin` lift the logic from `main.cpp:56–76` into the
sandboxed runtime library.

## MINOR

### 1. (§2.1, line 69) Inaccurate line citation for `UserPermascroll` declaration

Full title: Line citation `user_permascroll.hpp:25–65` points to `DeviceDelegation`, not
`UserPermascroll`

**Problem:** §2.1 cites `apps/common/xanadu/user_permascroll.hpp:25–65` for `UserPermascroll`. Lines
25–65 define `struct DeviceDelegation`. `class UserPermascroll` is declared at line 101.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:69`:

  > `apps/common/xanadu/user_permascroll.hpp:25–65`

- In `apps/common/xanadu/user_permascroll.hpp:38, 101`: Line 38: `struct DeviceDelegation` Line 101:
  `class UserPermascroll : public SpanReader`

**Fix:** Update citation to `apps/common/xanadu/user_permascroll.hpp:101–175`.

### 2. (§2.1, line 73) Inaccurate line citation for `SegmentedOpsSpool` declaration

Full title: Line citation `segmented_ops_spool.hpp:42–88` points to header constants, not class

**Problem:** §2.1 cites `segmented_ops_spool.hpp:42–88` for `SegmentedOpsSpool`. Lines 42–88 contain
format constants (`opsSegmentFormatVersion`, `opsSegmentHeaderBytes`) and `OpsSegmentHeader`
comments. `class SegmentedOpsSpool` is declared at line 220.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:73`:

  > `segmented_ops_spool.hpp:42–88`

- In `apps/common/xanadu/segmented_ops_spool.hpp:51, 220`: Line 51:
  `inline constexpr std::uint32_t opsSegmentFormatVersion = 2;` Line 220: `class SegmentedOpsSpool`

**Fix:** Update citation to `apps/common/xanadu/segmented_ops_spool.hpp:220–295`.

### 3. (§5.2, line 402) Inaccurate line range citation for `MetaRuntime::validate`

Full title: Line citation `meta.cpp:490–623` truncates `MetaRuntime::validate`

**Problem:** §5.2 cites `apps/common/xanadu/vagent/meta.cpp:490–623` for `MetaRuntime::validate`.
Lines 490–505 are helper methods `testOf` and `bodyOf`. `MetaRuntime::validate` begins at line 506
and extends through line 712.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:402`:

  > `Static Opcode Validation (apps/common/xanadu/vagent/meta.cpp:490–623)`

- In `apps/common/xanadu/vagent/meta.cpp:494, 506, 712`: Line 494:
  `std::optional<CellRef> MetaRuntime::testOf(CellRef clause)` Line 506:
  `MetaValidateReport MetaRuntime::validate(CellRef graphRootCell, ...)` Line 712: closing brace of
  `MetaRuntime::validate`

**Fix:** Update citation to `apps/common/xanadu/vagent/meta.cpp:506–712`.

### 4. (§3.2, lines 212-214) Precise C++ method citation for `resolve_dim`

Full title: Citation for `resolve_dim` should reference `findOrMintDimension` and `ensureDimension`

**Problem:** §3.2 describes `resolve_dim(name)` as "allocating an arena dimension if not yet
registered", but does not cite the exact methods in `VortexCore` or `ArenaManifold`.

**Evidence:**

- In `apps/common/xanadu/vortex/vortex_core.hpp:251`:
  `DimRef findOrMintDimension(std::string_view name);`

- In `apps/common/xanadu/zigzag/arena_manifold.hpp:352`:
  `DimRef ensureDimension(std::string_view name);`

**Fix:** Add explicit citations to `VortexCore::findOrMintDimension` (`vortex_core.hpp:251`) and
`ArenaManifold::ensureDimension` (`arena_manifold.hpp:352`) in §3.2.

### 5. (§3.2, lines 188-201) Class diagram type mismatch for fallible ZigZag primitives

Full title: `StdZigzagModule` class diagram shows bare `CellRef` instead of `Opt<CellRef>`

**Problem:** In §3.2's `StdZigzagModule` class diagram, methods such as `step`, `insert`, `unlink`,
and `link` are shown returning bare `CellRef`. This contradicts §3.1 (which lists `Opt<CellRef>`),
the live C++ signatures (`std::optional<CellRef>`), and §8.3 (which strictly refuses sentinel
integers in APIs).

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:188–194`: `+step(cursor, dim, dir) CellRef`
  `+insert(cursor, dim, dir, text) CellRef`

- In `apps/common/xanadu/vortex/vortex_stdlib.hpp:317–325`: `std::optional<CellRef> zzStep(...)`
  `std::optional<CellRef> zzInsert(...)`

**Fix:** Update the Mermaid diagram in §3.2 to show `Option[CellRef]` (or `Opt<CellRef>`) for all
fallible methods.

VERDICT: NOT CONVERGED
