# Round 1 Critique: Technical Writer (`writer`)

**Target Document:** `design/vagent-metacircular-compiler-implementation.md` **Companion
Documents:** `design/vagent-metacircular-compiler.md`, `design/vagent-metaprogramming.md`

## Findings

### [WRITER-1] BLOCKING: Conflation of registerHead signature and collision with meta.hpp

- **Location**:
  `design/vagent-metacircular-compiler-implementation.md:258-261, 409-410, 420-427, 465-467`;
  `apps/common/xanadu/vagent/meta.hpp:684`, `apps/common/xanadu/vagent/meta.cpp:2206`

- **Evidence**: The implementation plan declares at lines 258–261:

  ```cpp
  Declare MetaRuntime methods for Stage 2:
    - std::expected<CellRef, std::string> registerHead(CellRef head);
  ```

  and uses `meta.registerHead(head)` throughout Stage 2 as the new atomic registration verb.
  However, in `apps/common/xanadu/vagent/meta.hpp:684` and `meta.cpp:2206`,
  `MetaRuntime::registerHead(CellRef head) const` already exists and returns `MetaMutation`
  (`std::expected<void, std::string>`). It performs code-registration on `d.code`. In
  `design/vagent-metacircular-compiler.md:282-287, 416-418, 575-577`, the architecture document
  explicitly distinguishes kind-registration from code-registration:
  `"(a filed dim name beside d.code, distinct from registerHead's code-registration —"`
  `"the naming collision is real)"`. Re-declaring `registerHead` with return type
  `std::expected<CellRef, std::string>` fails C++ compilation (conflicting declaration differing
  only by return type) and destroys the distinction between `d.code` registration and the atomic
  kind+code `register` verb.

- **Fix**: Rename the new atomic verb method on `MetaRuntime` to `registerFragment(CellRef head)`
  (or `registerCode(CellRef head)`), returning `std::expected<CellRef, std::string>`, and retain
  `registerHead(CellRef head)` as the internal helper responsible solely for `d.code` registration.

### [WRITER-2] BLOCKING: Hallucinated member programOpcodes\_ vs real opcodeMap\_

- **Location**:
  `design/vagent-metacircular-compiler-implementation.md:18-20, 147, 407, 420, 424, 427, 447, 465`;
  `design/vagent-metacircular-compiler-implementation.md:490, 763`;
  `apps/common/xanadu/vortex/vortex_vm.hpp:269`
- **Evidence**: Line 18–20 states:
  `"Because Vortex >Value and >Link instructions cannot set the out-of-band"`
  `"VortexVM::programOpcodes_ table (apps/common/xanadu/vortex/vortex_vm.hpp:239), the entire"`
  `"architecture hinges on one narrow trusted primitive..."` and the document repeatedly refers to
  mutating or querying `vm_.programOpcodes_`. In reality, `programOpcodes_` does not exist anywhere
  in the codebase. The VM's registered opcode table is `opcodeMap_`, declared at
  `apps/common/xanadu/vortex/vortex_vm.hpp:269` as
  `std::unordered_map<CellRef, OpcodeKind> opcodeMap_;`. Line 239 of `vortex_vm.hpp` is part of the
  unrelated `ExecutionResult run(...)` signature. The companion architecture document
  `design/vagent-metacircular-compiler.md` (lines 184, 188, 203, 212, 350, 402, 414, 579, 613, 622,
  675, 683) consistently uses the correct name `opcodeMap_` and cites `vortex_vm.hpp:269` at line
  212\.
- **Fix**: Replace all occurrences of `programOpcodes_` with `opcodeMap_`, and correct the citation
  to `apps/common/xanadu/vortex/vortex_vm.hpp:269`.

### [WRITER-3] BLOCKING: Erroneous `kMetaDimensionNames` definition deletes 6 core dimensions

- **Location**: `design/vagent-metacircular-compiler-implementation.md:244-254`;
  `apps/common/xanadu/vagent/lineage.hpp:58-62`

- **Evidence**: Lines 244–254 state:
  `"Extend kMetaDimensionNames (apps/common/xanadu/vagent/lineage.hpp:58-62) from 6 entries to 8"`
  `"entries by filing \"d.lineage\" (P5) and \"d.kind-registered\" (P4):"` followed by an array
  definition with only 8 dimensions. In reality, `apps/common/xanadu/vagent/lineage.hpp:58-62`
  defines 12 entries:

  ```cpp
  inline constexpr std::string_view kMetaDimensionNames[] = {
      "d.code",  "d.patch",      "d.weight",   "d.synapse",
      "d.entry", "d.patch-head", "d.validate", "d.refs",
      "d.ref",   "d.recipe",     "d.refFor",   "d.mesh.relocation",
  };
  ```

  The snippet in the implementation plan deletes six core dimensions: `"d.weight"`, `"d.synapse"`,
  `"d.entry"`, `"d.validate"`, `"d.recipe"`, and `"d.mesh.relocation"`. Furthermore, it introduces
  typos for existing dimensions: `"d.patch.head"` (real: `"d.patch-head"`) and `"d.ref.for"` (real:
  `"d.refFor"`).

- **Fix**: Update the baseline count to 12 entries and extend `kMetaDimensionNames` from 12 to 14
  entries by appending `"d.lineage"` and `"d.kind-registered"`, preserving the exact spellings
  `"d.patch-head"` and `"d.refFor"`.

### [WRITER-4] BLOCKING: Contradiction on pending vs carrier rank membership during pending window

- **Location**: `design/vagent-metacircular-compiler-implementation.md:431-432, 450-452`;
  `design/vagent-metacircular-compiler.md:284-286, 379-381, 387-390`
- **Evidence**: `design/vagent-metacircular-compiler-implementation.md:431-432` states:
  `"Link a trailed \"pending\" status marker on head's d.vars rank ($reg_state → \"pending\") and"`
  `"append head to a trailed pending rank (or d.kind-registered with $reg_state == \"pending\")."`
  And line 451 states:
  `"For each head $h_i$ that failed Pass 1... unlink $h_i$ from d.kind-registered..."` However,
  `design/vagent-metacircular-compiler.md:387-390` explicitly rules:
  `"The carrier write *is* the commit act — a drain-faulted head's entry is never appended,"`
  `"so registration reads absent..."` and lines 379–381 state:
  `"in the pending window map and carrier agree (both absent — pending is the third state"`
  `"precisely because a Bool couldn't express it). The durable half needs no verb at all — the"`
  `"carrier rank is Linked-walkable."` Allowing pending heads onto `d.kind-registered` violates the
  invariant that `d.kind-registered` contains only validated, committed heads. Any rank-walk during
  the turn would observe uncommitted heads.
- **Fix**: Remove `"(or d.kind-registered with $reg_state == \"pending\")"`. Specify that pending
  registrations queue exclusively on a trailed pending list on the agent cell (e.g.
  `d.pending-registers`), and heads are appended to `d.kind-registered` only during Pass 2 of
  `drainPendingRegisters()` upon passing validation.

### [WRITER-5] MAJOR: Missing Proposal Register entry for Stage 1 (`std:meta` Lowering & Dispatch)

- **Location**: `design/vagent-metacircular-compiler-implementation.md:169, 271-329, 754-771`
- **Evidence**: Section 3 Stage 1 ("Lowering & Executor Wiring for the `std:meta` Surface", Track C)
  modifies existing engine files `apps/common/xanadu/vagent/compiler.cpp` (`compileCall`),
  `apps/common/xanadu/vagent/decls.cpp` (`mintHostStep`), and
  `apps/common/xanadu/vagent/executor.cpp` (`runHostStep`), adding lowering and host-step dispatch
  for over 15 `std:meta` verbs. However, in Section 5's Proposal Register (`P1`–`P12`), Stage 1 has
  no proposal entry. The repository standing rules (`brief-common.md`, `AGENTS.md`) mandate:
  `"Every change to existing code must be tracked in the numbered Proposal Register (P<k>)."`
- **Fix**: Add a dedicated proposal `P13` (or `P0` / `P1a`) in the Proposal Register tracking the
  modifications to `compiler.cpp`, `decls.cpp`, and `executor.cpp` for Stage 1's `std:meta` lowering
  and `runHostStep` dispatch, pricing the cycle/LoC costs and documenting refused alternatives.

### [WRITER-6] MAJOR: Incomplete companion document citation and omission of decisions D6 and D7

- **Location**: `design/vagent-metacircular-compiler-implementation.md:3, 52`
- **Evidence**: Line 3 cites:
  `"Companion to: design/vagent-metacircular-compiler.md (architecture & §0–§5, D1–D5, P1–P8)"`
  `design/vagent-metacircular-compiler.md` has no `§0` (sections run §1 through §7) and defines
  seven decisions: `D1` through `D7` (`D6 — Arena discipline`, line 619; `D7 — Type story`, line
  631). Omitting D6 and D7 causes downstream specification gaps:
  - Under D6, committed-turn AST garbage persists in the arena until `promote` or next incarnation;
    the implementation plan does not account for this memory retention in its fuel and allocation
    bounds.
  - Under D7, runtime-compiled fragments type as `Code<Cell>`, which cannot reach
    `Code<()>`-invariant parameters (e.g. `learn_body`, `graft_body`) without `as_body(c)`. Stage
    3's exit gate (line 527) calls `learn_body` directly on the output of `compile_fragment`
    (`Opt<Code<Cell>>`) without `as_body`.
- **Fix**: Update the companion citation to `(architecture & §1–§7, D1–D7, P1–P8)`. Update Stage 3
  and Stage 5/6 to account for D6 arena garbage retention and D7 `as_body` narrowing requirements.

### [WRITER-7] MAJOR: Fabrication of citations and non-existent section numbers in companion doc

- **Location**: `design/vagent-metacircular-compiler-implementation.md:146, 273, 503, 536, 537`;
  `design/vagent-metacircular-compiler-implementation.md:601, 647, 682, 686-688`
- **Evidence**: The implementation plan repeatedly cites non-existent sections in
  `design/vagent-metacircular-compiler.md`:
  - Line 146: cites `§3.4` (Section 3 only has §3.1, §3.2, §3.3; deferral is in §3.2).
  - Line 273: cites `§1.1` (Section 1 has no subsections; the emit-arm gap is in Section 2).
  - Line 503: cites `§4.1, Stage 3` (Section 4 has no §4.1; stages are under D1).
  - Line 536: cites `§3.6, P7` (Section 3 has no §3.6; batched join is under §3.2).
  - Lines 537, 601, 647, 682: cite `§4.1` and `§4.2`.
  - Lines 686–688: fabricates a direct quote:
    `As noted in design/vagent-metacircular-compiler.md §4.1 (Stage 7: "Needs a string/char surface`
    `(str_len, char_at or chars rank, str_slice) and int_to_str")` — this quote does not appear in
    `vagent-metacircular-compiler.md`.
- **Fix**: Replace all fabricated subsection numbers with the real section and decision headings
  (e.g. `§3.2`, `§2`, `§4 D1 (Stage 3)`, `§3.2 (Q7/P7)`), and rephrase lines 686–688 to attribute
  `P12`'s motivation to the real prose in `vagent-metacircular-compiler.md:487-500`.

### [WRITER-8] MAJOR: Systematic citation error for `std:meta` typing in `checker.cpp`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:273-274, 348, 763`;
  `apps/common/xanadu/vagent/checker.cpp:966-1025`
- **Evidence**: The implementation plan repeatedly cites
  `apps/common/xanadu/vagent/checker.cpp:1049-1231` as the location where `std:meta` functions are
  typed and where new verbs should be added. In reality, `checker.cpp:1049-1231` contains
  `Checker::checkAction` and action-statement checking. `std:meta` builtin signatures are declared
  at `apps/common/xanadu/vagent/checker.cpp:966-1025` (with special forms `metaRun` and `metaReify`
  at lines 1165–1250). The companion document `design/vagent-metacircular-compiler.md:86, 289`
  correctly cites `checker.cpp:970-1020` and `checker.cpp:966-1020`.
- **Fix**: Correct all citations in lines 273, 348, and 763 from `checker.cpp:1049-1231` to
  `apps/common/xanadu/vagent/checker.cpp:966-1025` (and `:1165-1250`).

### [WRITER-9] MAJOR: Stage 1 / Stage 4 ordering contradiction regarding `join_batch`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:166-183, 311-312, 558-571`
- **Evidence**: Under Wave 1, Track C (Stage 1) is scheduled in parallel with Stage 2, before
  Barrier 1. At lines 311–312, Stage 1 specifies lowering:
  `"Synthesis & registration: reify, register, register_all, and Stage 4's batched join verb."`
  However, the batched join verb (`join_batch`) is specified, designed, and implemented in **Stage
  4** (Track F, Wave 2, lines 558–571). Furthermore, Stage 1's exit gate (lines 324–327) requires
  compiling and running programs that call every `std:meta` verb. Track C in Wave 1 cannot lower or
  test `join_batch` before `join_batch` is implemented in Stage 4.
- **Fix**: Clarify that Stage 1 lowers and dispatches only existing `std:meta` verbs plus Stage 2's
  registration verbs. Lowering and dispatch of `join_batch` belongs to Track F / Wave 2 in Stage 4
  alongside the bootstrap pair `boot:writer` and `boot:linker`.

### [WRITER-10] MAJOR: Ambiguous R1 AST topology violates or obscures ZigZag 2-degree rank invariant

- **Location**: `design/vagent-metacircular-compiler-implementation.md:578-580`
- **Evidence**: Line 580 states:
  `"Each R1 AST node is a cell whose text (or +d.verb link to the filed d.ast.kinds cell) names"`
  `"its kind, +d.name holds its identifier/operator/literal token, +d.values holds its annotated"`
  `"type cell (populated by Stage 6), and +d.step chains its child AST nodes in order."` In ZigZag,
  `d.step` is a 1D dimension where each cell has at most one posward and one negward neighbor. If a
  parent AST node links posward on `+d.step` to its first child, and that child links posward on
  `+d.step` to its sibling, then if the child also has its own children, it cannot link posward on
  `+d.step` to both its sibling and its first child without evicting a link. Standard ZigZag AST and
  descriptor structures (e.g. `fragment.cpp:303-317`) link parent to the head of the child rank via
  `+d.grab` (or `+d.children`), and chain siblings posward on `+d.step`.
- **Fix**: Specify the explicit 2-degree AST rank schema: Parent links posward via `+d.grab` to its
  first child; siblings chain posward on `+d.step`; leaf nodes terminate with `noCell` on `+d.step`.

### [WRITER-11] MINOR: Omission of `prelude/meta.vagent` from implementation steps

- **Location**: `design/vagent-metacircular-compiler-implementation.md:211-217, 348-356, 556-557`;
  `apps/common/xanadu/vagent/prelude/meta.vagent`
- **Evidence**: `apps/common/xanadu/vagent/prelude/meta.vagent` defines `module std:meta` and
  `capacity Meta` for the VAgent surface language. While the implementation plan specifies adding
  `register`, `register_all`, `registration`, `dim_named`, `as_body` (Stage 2) and `join_batch`,
  `boot_writer`, `boot_linker` (Stage 4) to C++ `checker.cpp`, `meta.cpp`, and `compiler.cpp`, it
  never lists `apps/common/xanadu/vagent/prelude/meta.vagent` in the file ownership matrix or tasks.
  VAgent programs importing `std:meta` will fail capacity resolution if the prelude signatures are
  missing.
- **Fix**: Add `apps/common/xanadu/vagent/prelude/meta.vagent` to Track C / Stage 1 and Track A /
  Stage 2 in the File-Ownership Matrix, and specify the capacity method additions for all new
  `std:meta` verbs.

### [WRITER-12] MINOR: Erroneous line citation for `m0` and `failTurn` in `executor.cpp`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:428-433`;
  `apps/common/xanadu/vagent/executor.cpp:256-265, 695`
- **Evidence**: Line 429 states: `"(the normal turn execution path inside m0 at executor.cpp:695)"`,
  and line 433 states: `"(failTurn at executor.cpp:702)"`. In
  `apps/common/xanadu/vagent/executor.cpp`, lines 695–716 define `refTypedResult`, an internal
  lambda for checking whether a produced Int64 is a ref-box. The actual creation of `m0` occurs at
  `executor.cpp:257` (`const auto m0 = arena.mark();`), and `failTurn` is defined at lines 260–265
  (`auto failTurn = [&]() -> ... arena.release(m0);`).
- **Fix**: Correct the citations in lines 429 and 433 from `executor.cpp:695-702` to
  `executor.cpp:256-265`.

### [WRITER-13] MINOR: Missing filing of dimension `"d.ast.kinds"` in `lineage.hpp`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:573-577`;
  `apps/common/xanadu/vagent/lineage.hpp:58-62`
- **Evidence**: Line 573 introduces a filed dimension `d.ast.kinds` on `programRoot` for R1 AST kind
  tags:
  `"File the canonical AST kind-tag rank on programRoot under a filed dimension d.ast.kinds..."`
  However, `d.ast.kinds` is not listed in `kMetaDimensionNames` in Stage 0 (line 248) or anywhere in
  `lineage.hpp`. Unfiled dimensions cannot be swept or safely referenced across checkpoints.
- **Fix**: Add `"d.ast.kinds"` to `kMetaDimensionNames` in `lineage.hpp` in Stage 0 / Stage 4,
  pricing the 1 additional dimension cell.

### [WRITER-14] NIT: Markdown formatting and linting violations (`mdl` and `mdformat`)

- **Location**: `design/vagent-metacircular-compiler-implementation.md:20, 31, 247, 485, 590, 662`
- **Evidence**: Running `mdl -i design/vagent-metacircular-compiler-implementation.md` yields:
  - 58 lines exceeding the 100-character column limit (`MD013`).
  - Missing blank lines surrounding fenced code blocks (lines 247, 252, `MD031`).
  - Missing blank lines surrounding lists (lines 485, 590, `MD032`).
  - Unescaped type parameters parsed as raw HTML (e.g. line 662: `Opt<T>`, `[T]`, `MD033`).
  - Spaces inside code span elements (line 662: `` ` ()` ``, `MD038`).
- **Fix**: Reflow text to `--wrap 100`, insert required blank lines before/after code blocks and
  lists, escape `<T>` as `\<T\>` or fence properly in code spans without stray spaces (`` `()` ``).

______________________________________________________________________

SUMMARY: BLOCKING=4 MAJOR=6 MINOR=3 NIT=1

VERDICT: NEEDS-REVISION
