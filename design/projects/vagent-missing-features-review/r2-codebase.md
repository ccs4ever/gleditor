# Round 2 critique: Codebase Expert (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 1 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.B1 (Hallucinated `consumeWork(file_byte_size)` signature): RESOLVED — Parameterless
  `consumeWork()` cycle accounting restored and memory bounding delegated to scratch allocation
  ceiling (`ArenaManifold::setExecutionLimits`).
- R1.B2 (Hallucinated `ActorError::GrantExceeded` enum variant): RESOLVED — Replaced with canonical
  `ActorError::ResourceLimit`.
- R1.B3 (Contradiction of `ValueKind` and `CompactOpNode` invariants): RESOLVED — Primedia text
  cleanly segregated into `set_text` (`setContent`) while `set_value` handles only scalar bits
  (`setValueBits`).
- R1.B4 (`std:zigzag:promote` signature and store mutability): RESOLVED — Proposal P27 makes
  `boundStore_` mutable `Store*`, `parentVersion` is derived from `primaryCurrentVersion()`,
  `extraRoots` includes `d.dims` and relocation targets, and return type is
  `Result<Str, ZigzagError>`.
- R1.M1 (Incomplete file inventory in Proposal P30): RESOLVED — Proposal P30 file inventory expanded
  to include `token.hpp:15–53`, `token.cpp`, `lexer.cpp`, `stdlib/lexer.vagent`, `ast.hpp`,
  `r1_ast.hpp/.cpp`, `stdlib/ast.vagent`, `checker.cpp`, `stdlib/checker.vagent`, `compiler.cpp`,
  and `stdlib/emitter.vagent`.
- R1.M2 (Layer boundary violation in Proposal P29 `std:actor:spawn_image`): RESOLVED — Dynamic actor
  spawning removed from generic `vortex_stdlib.cpp` and positioned at the VAgent adapter/executor
  layer (`VagentAdapter`, `adapter.hpp`, `executor.cpp`).
- R1.M3 (Missing host argument injection mechanism in Proposal P26): RESOLVED — Proposal P26 adds
  `VortexStdLib::setArgs` and updates `apps/vagent/main.cpp` to forward positional CLI arguments
  into the execution context.
- R1.M4 (Missing citation of existing host file ingestion in `main.cpp:56–76`): RESOLVED — §2.1 and
  Proposal P26 explicitly cite `apps/vagent/main.cpp:56–76` (`readSource`).

## BLOCKING

(none)

## MAJOR

(none)

## MINOR

### 1. (§7, line 518) Minor line citation transposition in Proposal P29

Full title: Line citation `adapter.cpp:275–296` transposes line range from
`apps/vagent/main.cpp:275–296`

**Problem:** In §7, Proposal P29 lists `apps/common/xanadu/vagent/adapter.cpp:275–296`. Lines
275–296 in `apps/common/xanadu/vagent/adapter.cpp` handle `ChildDown` and `ChildStopping` percept
processing. The cited line range 275–296 originates from `apps/vagent/main.cpp:275–296`, where
`adapter.setSpawnSender` is wired into `runtime.spawn`. In `adapter.cpp`, `setSpawnSender` is
defined at line 144.

**Evidence:**

- In `design/vagent-metacircular-missing-features.md:518`:

  > `P29 | apps/common/xanadu/vagent/adapter.cpp:275–296, adapter.hpp, executor.cpp | ...`

- In `apps/common/xanadu/vagent/adapter.cpp:144`:

  ```cpp
  VagentAdapter *VagentAdapter::setSpawnSender(SpawnSender sender) {
  ```

- In `apps/vagent/main.cpp:275–296`:

  ```cpp
  adapter.setSpawnSender(
      [&](WorkerContext &ctx, zigzag::CellRef /*member*/,
          std::string_view requestId)
          -> std::expected<void, ActorError> {
        ...
        auto result = runtime.spawn(identity, request, wallNow,
                                    ActorLineage{fold.get(), &lineage});
        if (!result) return std::unexpected(result.error());
        adapter.spawnOutcome(ctx, request, result);
        return {};
      });
  ```

**Fix:** Update Proposal P29's file column to cite `apps/vagent/main.cpp:275–296`,
`apps/common/xanadu/vagent/adapter.cpp:144`, `adapter.hpp`, and `executor.cpp`.

VERDICT: CONVERGED
