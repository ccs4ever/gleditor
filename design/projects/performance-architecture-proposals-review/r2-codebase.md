# Round 2 critique: Codebase Expert (performance-architecture-proposals.md)

Paths below are relative to the repo root (`/data/git/gleditor-restore`). Tally: 0 BLOCKING, 0
MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- **Finding 1: Missing Proposal Register for C++ engine changes (House Style Rule 6)** —
  **RESOLVED**
  - *Evidence:* Section 3 in v2 introduces a formal numbered Proposal Register (`P1..P5`) detailing
    exact files touched, architectural justification for why the logic cannot reside in caller or
    Vortex stdlib, concrete priced costs, and explicit fallbacks.
- **Finding 2: Manifold LMDB checkpointing ignores `byRef` micro-history chains** — **RESOLVED**
  - *Evidence:* LMDB `Manifold` checkpointing has been completely refused and removed from the
    active proposals. In §4 (*Refused Alternatives* #2), the document explicitly records that LMDB
    `Manifold` checkpoints violate R9 materialised-view honesty and OSMIC DAG branching, and records
    that `PrefixSnapshot` omits `byRef` and `structureBirths_`.
- **Finding 3: `MDB_INTEGERKEY` specified for composite `{StoreId, opIndex}`, and `StoreId` is
  hallucinated** — **RESOLVED**
  - *Evidence:* Struck with the refusal of LMDB checkpoints. `LMDBContentCache` is correctly cited
    at `lmdb_cache.hpp:93` in §4.2 as a security-isolated BitTorrent content and CEK cache.
- **Finding 4: Proposal 4 cites non-existent `MmapArena`, ignores existing inline `base()`, and
  falsely claims `size()` needs inlining** — **RESOLVED**
  - *Evidence:* In §2.4 and Proposal Register `P4`, `VirtualMemoryArena::base()`
    (`virtual_memory_arena.hpp:97`) and `SegmentedOpsSpool::size()` (`segmented_ops_spool.hpp:290`)
    are accurately acknowledged as already public inline in their headers. The proposal cleanly
    focuses on moving `SegmentedOpsSpool::get(uint32_t)` and `ArenaManifold::slot()` into headers.
- **Finding 5: Misstated `ArenaManifold::linked` behavior regarding `denseOf` and `isProxy` hash
  lookups** — **RESOLVED**
  - *Evidence:* In §2.3, the analysis is corrected to note that `ArenaManifold::denseOf(from)` is
    direct bitwise arithmetic for ephemeral cells. The redesign reorders `ArenaManifold::linked` to
    check `slots_[d].valueKind == ValueKind::ExternRef` first, completely bypassing
    `isQuoteOccurrence(from)` and `occurrenceToCanonical_` probes on >99% of calls without any side
    tables or bit-packing.
- **Finding 6: Hallucinated class name `LmdbCache` and unargued bit-packing under House Style Rule
  2** — **RESOLVED**
  - *Evidence:* Corrected to `LMDBContentCache` in §4.2. Bit-packing into `CellSlot::flags` has been
    abandoned in §2.3 in strict compliance with House Style Rule 2 and preserving
    `sizeof(CellSlot) == 32` (`manifold.hpp:357`).

______________________________________________________________________

## BLOCKING

(None)

______________________________________________________________________

## MAJOR

(None)

______________________________________________________________________

## MINOR

### 1. (§3, line 237) P5 Register omission of `vortex_frames.cpp` and `vortex_vm.hpp`

Full title: P5 register lists `vortex_vm.cpp` but omits `vortex_frames.cpp` where
`VortexVM::inputsOf` and `VortexVM::outputsOf` are defined

**Problem:** The Proposal Register entry for `P5` lists
`apps/common/xanadu/vortex/vortex_core.{hpp,cpp}, vortex_vm.cpp`. However, `VortexVM::inputsOf` and
`VortexVM::outputsOf` are declared in `apps/common/xanadu/vortex/vortex_vm.hpp:792-793` and defined
in `apps/common/xanadu/vortex/vortex_frames.cpp:326-335`. Whether `VortexVM::inputsOf` is updated to
return `inplace_vector` or merely consumes the new `inplace_vector` from `core_.inputsOf`,
`vortex_frames.cpp` and `vortex_vm.hpp` are affected.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_frames.cpp:326-335`:

  ```cpp
  std::vector<CellRef> VortexVM::inputsOf(const CellRef opcode) const {
    auto slots = core_.inputsOf(opcode);
    for (auto &slot : slots) slot = runtimeSlot(slot);
    return slots;
  }
  ```

- `apps/common/xanadu/vortex/vortex_vm.hpp:792-793`.

**Fix:** Add `apps/common/xanadu/vortex/vortex_frames.cpp` and `vortex_vm.hpp` to the "Files
Touched" column for `P5` in the Proposal Register.

______________________________________________________________________

### 2. (§2.5, line 215 & §5, line 272) Review finding

Full title: Use canonical `gleditor::cpp26::inplace_vector` alias per `AGENTS.md` rather than
namespace `beman::`

**Problem:** §2.5 line 215 and §5 line 272 cite `beman::inplace_vector<CellRef, 8>`.
`AGENTS.md:231-236` and `design/cpp26-compatibility.md` mandate that C++26 facilities come from
`<gleditor/cpp26*.hpp>` as `gleditor::cpp26::*`, which selects between native `std::inplace_vector`
and `beman::inplace_vector` via `cpp26_select.hpp`.

**Evidence:**

- `AGENTS.md:231-236`:
  > "C++26 library facilities (`function_ref`, `optional<T&>` and optional-as-range,
  > `inplace_vector`, two-span `views::concat`, checked `span_at`) come from `<gleditor/cpp26*.hpp>`
  > as `gleditor::cpp26::*`, in the library and the apps alike."
- `include/gleditor/cpp26_inplace_vector.hpp:14-23`.

**Fix:** Update `beman::inplace_vector` to `gleditor::cpp26::inplace_vector`.

______________________________________________________________________

### 3. (§2.1, lines 82-84) Review finding

Full title: Explicitly record that `abortScopedChoices` and `abortOwnedMutationGroups` restore state
via `vmTrail_` watermark

**Problem:** In §2.1 lines 82-84 and lines 86-91, `backtrack()`, `cut()`, and `VMSavepoint` are
discussed. In `apps/common/xanadu/vortex/vortex_vm.cpp`, `abortScopedChoices` (lines 1548-1579) and
`abortOwnedMutationGroups` (lines 468-509) also unwind choice points and swap 10 maps. They are
direct beneficiaries of unwinding `vmTrail_` back to `saved.vmTrailMark`.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_vm.cpp:468-509` (`abortOwnedMutationGroups` discards
  intermediate marks and swaps the 10 maps with `saved`).
- `apps/common/xanadu/vortex/vortex_vm.cpp:1548-1579` (`abortScopedChoices` discards intermediate
  marks and swaps the 10 maps with `saved`).

**Fix:** Mention `abortScopedChoices` and `abortOwnedMutationGroups` alongside `backtrack()` as
choice-point unwind paths unified by `vmTrail_`.

______________________________________________________________________

### 4. (§2.4, line 190) Move `Manifold::slot()` to header alongside `ArenaManifold::slot()`

Full title: Include `Manifold::slot()` in P4 header inlining

**Problem:** Proposal 4 lines 190-191 states: "Move `SegmentedOpsSpool::get(std::uint32_t)` (mutable
and const) and `ArenaManifold::slot()` into `segmented_ops_spool.hpp` and `arena_manifold.hpp` as
inline methods." `Manifold::slot()` (`apps/common/xanadu/zigzag/manifold.cpp:136-139`) is also
called frequently across replay and traversal. Inlining `Manifold::slot()` in `manifold.hpp`
provides symmetric benefits.

**Evidence:**

- `apps/common/xanadu/zigzag/manifold.hpp:369`:
  `[[nodiscard]] SlotRef slot(CellRef ref) const noexcept;`

- `apps/common/xanadu/zigzag/manifold.cpp:136-139`:

  ```cpp
  SlotRef Manifold::slot(const CellRef ref) const noexcept {
    const auto dense = denseOf(ref);
    return !dense.has_value() ? SlotRef{} : SlotRef{slots[*dense]};
  }
  ```

**Fix:** Add `Manifold::slot()` in `manifold.hpp` to the list of accessors inlined in P4.

______________________________________________________________________

VERDICT: CONVERGED
