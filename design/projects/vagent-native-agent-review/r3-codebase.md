# Round 3 critique: Codebase Expert (vagent-native-agent.md v0.3)

Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.B1 (compiler refuses native skills and effects): RESOLVED as prerequisite Q1 (§1.1). The scope
  matches the code: `compiler.cpp:6761-6763`, `:6102`, `:6822` refuse; `Gen::effectCall` (`:3827`)
  has no caller and writes no `capacity`/`deadline` (`kOutboxEffectCapacity` and
  `kOutboxEffectDeadline` are read at `agent_executor.cpp:2377-2379` and never written by the
  compiler); `ANativeSkillIsRefusedByName` is at `vagent_skill_test.cpp:371`. The gate is checkable.
- R2.M1 (`kProgramFormat` in `programRef`): RESOLVED (`program.hpp:36-39` verified).
- R2.M2 (ledger permascroll): RESOLVED (§10.5 private permascroll; `store.cpp:123-125` is the
  default in-memory `Store()`; `system_docs.hpp:760-765` scalar-only claim holds).
- R2.M3 (control-side glob): RESOLVED (P20.5 `filter-out` plus `native_unavailable.cpp`).

## Citations checked

All new v0.3 citations hold: `program.hpp:36-39`, `ast.hpp:138-141` (`ECall` has no `orElse`; only
`achieve`/`ask`/`await` do, `ast.hpp:233-246`, `parser.cpp:1466-1499`),
`agent_executor.cpp:2379-2392` (the missing-field `kStatMalformed` skip), `:4770-4827` (`checkpoint`
leaves the agent running), `:3895` (`bindings_->start`), `mesh_actor.cpp:241-243` and `:322-324`
(`WrongEpoch`), `store.cpp:123-125`, `agent_executor.hpp:347-351`, `:410-413`. P20.1-P20.8 cover the
existing-code changes I can find; no unlisted edit to `ast.hpp`, `checker.cpp`, `mesh_*` or
`host_bindings` is needed.

## BLOCKING / MAJOR

None.

## MINOR

- (§1.1 Q1) `effectCall` already lowers a plan's effect call as a suspension point with step entry
  and `Waiting` record (`compiler.cpp:3870-3900`). Q1's "lower a plan's effect call as a suspension
  point with its step path" overstates the gap; the real work there is wiring and tests. The missing
  pieces are the capacity name, the deadline and result decoding.
- (§1.1 Q1) `effectCall` writes no `kOutboxEffectPreempts` either; say whether Q1 leaves it unset.
- (§12.3) The `checkpoint` range ends a few lines after `:4827`; use `:4770-4836`.
- (P20.2) State that `fromUnit` takes `Unit` by value (move-only, `ast.hpp:26`), so the replay
  result is moved in.
