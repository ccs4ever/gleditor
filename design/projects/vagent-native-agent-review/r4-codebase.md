# Round 4 critique: Codebase Expert (vagent-native-agent.md v0.4)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous findings and v0.4 codebase audit

- **Previous BLOCKING/MAJOR status:** All previous findings (R2.B1/Q1 effect/skill compiler
  lowering, R2.M1 `kProgramFormat`, R2.M2 authority ledger private permascroll, R2.M3 Linux gating
  and `native_unavailable.cpp`) remain fully resolved. R3 minor findings were acknowledged in
  `r3-disposition.md` for final polish.
- **`deriveSchedule` citations (`agent_executor.cpp:2187`, `:4038-4072`):** Verified against the
  live code. Turn step 11 calls `deriveSchedule(state)` at `:2187` after outbox draining (`:2182`)
  and turn counter bump (`:2185`). The implementation at `:4038-4073` iterates over
  `intentionCells(arena, state.dims, state.agent.agentCell)` to populate `schedule.ready` and
  `schedule.deadlines`. Computing the set difference of live top-level intention IDs here to derive
  `intentionEnded(pid, intention)` (P20.6) operates on existing in-memory state without any
  additional manifold traversal or graph modification.
- **`d.subgoal` chain walk for ancestor resolution (N13, §4, P20.1):** Verified. In
  `agent_executor.cpp:108-113`, top-level intentions are linked along `dims.intention`, while
  subgoals extend posward along `dims.subgoal` (`rankAfter(arena, top, dims.subgoal)`). Walking
  `dims.subgoal` negward (`DimVector::NEG`) from a staging subgoal cell until the link is empty or
  hits the top-level cell reliably yields the root top-level intention ID without link eviction or
  side tables.
- **P20.6 additions to `AgentExecutor`:** Verified. `AgentExecutor` holds `std::string localNode_`
  (`agent_executor.hpp:682`, initialized at `:215`, `:920`), but currently lacks an accessor; adding
  `localNode()` is clean. The lifecycle observer (`stopped`, `intentionEnded`, `upgraded`)
  complements existing `IntentionObserver` (`:347-351`) and `TurnObserver` (`:410-413`).
- **Rewritten §6.4 (`vagent_native.hpp` / `vagent_native.h`) and AST alignment:** Verified.
  `vagent_native.h` (C ABI) and `vagent_native.hpp` (C++ wrapper) are header-only interfaces in
  `apps/vagent-native-host/`. The builder patterns in §6.4 and §13.1 (`plan.let`, `b.call`,
  `b.stmt`, `b.emit`, `b.record`, `b.name`, `b.lit`) map directly to `ast.hpp` types (`PlanDecl`,
  `Stmt`, `ECall`, `EEmit`, `ERecord`, `ELiteral`, `EName`). Packaging in P20.5 properly lists
  `Makefile:1334` (`TEST_PROGRAMS`), `Makefile:633` (`COMMON_XANADU_SRCS`), and packaging
  directories (`debian/`, `packaging/`).
- **`hold_terminal` in N13 / §10.1:** Verified. Synthesized as
  `effect def hold_terminal(t: Terminal) within 1s` in `capacity Terminals` alongside
  `open_terminal`, `close_terminal`, and `watch_terminal`. Calling it promotes the broker's
  in-memory grant from intention-scoped to agent-scoped.
- **`none_if` modifier and §11.6 `Keyboard` example:** Verified against `checker.cpp`. In §11.6:
  1. Assigning `t: Terminal` to `screen: Opt<Terminal>` (`screen := t`) is permitted by
     `checker.cpp:484-489` (`assignable` unwraps `expected` when `Opt`).
  1. `let t := screen` unwraps `screen` to `Terminal` via `checker.cpp:536`, `:2419`.
  1. `read_key(t)` returns `Opt<Int>` due to `none_if eq -1` on `fgetc`.
  1. `if key then revise last_key(key)`: `if key` checks `Opt` presence; inside `last_key(key)`,
     evaluating `key` invokes `node(const EName &n, ...)`, which unwraps `Opt<Int>` to `Int`
     (`checker.cpp:1760-1761`), matching `predicate last_key(code: Int)`. The entire agent
     type-checks cleanly without modifications to `checker.cpp`.
- **Proposal register P20.1–P20.8 accounting:** Verified. Every modification to existing
  engine/runtime code (`agent_executor.{hpp,cpp}`, `host_bindings.{hpp,cpp}`, `program.{hpp,cpp}`,
  `provenance.cpp`, `main.cpp`, `system_docs.cpp`, `Makefile`) is accounted for with exact
  citations, rationale, costs, and fallbacks.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- (§14.2 line 1206, P20.1) **P20.1 table summary wording:** The summary table states "the staging
  intention's id, absent for a handler", whereas N13 specifies that for subgoals, the negward
  `d.subgoal` walk maps the staging subgoal to its root top-level ancestor ID. Clarify the summary
  text to "the staging intention's resolved top-level ancestor id".
- (§12.4 line 1005) **`deriveSchedule` line range:** The method definition in `agent_executor.cpp`
  runs from `:4038` to `:4073` (line 4073 is the closing brace). Use `:4038-4073`.
- (§6.4 & §14.1) **Public installation path:** The document names `vagent_native.h` and
  `vagent_native.hpp` under `apps/vagent-native-host/`. Clarify whether `make install` and packaging
  rules place them in `<vagent/vagent_native.h>` or directly in `<vagent_native.h>`.
- (§10.1) **`hold_terminal` idempotency:** §10.1 specifies that `hold_terminal` on a revoked token
  fails `stale`. Explicitly specify that calling `hold_terminal` on an already held grant is an
  idempotent no-op.

VERDICT: CONVERGED
