# Round 1 disposition (performance-architecture-implementation-plan.md)

All seven reviewers used Luna. Writer and language ergonomics converged without findings. The other
five emitted no BLOCKING and overlapping MAJOR findings. V1 is revised to v2; every finding is
accepted after source verification. No runtime implementation or benchmark is claimed.

| Finding                                                                       | Ruling   | Applied correction in v2                                                                                                                                                                                                                                                                                                                                                                                                                       |
| ----------------------------------------------------------------------------- | -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| cpp-performance M1: allocation-free undo precedes reversible storage          | Accepted | M5a first delivers and independently wires/tests the reversible map/set adapter while preserving the snapshot algorithm. Trail rollback is enabled only after retained erased-key/rehash/clear capacity contracts pass. M5c is tuning/remaining maps and snapshot retirement. Prices include adapter storage.                                                                                                                                  |
| codebase M1, architect M1: P1 register omits resume/comparison TUs            | Accepted | Add vortex_resume.cpp, vortex_comparison.cpp and existing resume/comparison tests to P1. Explicitly adapt concrete helper types and test failed resume initialization while keeping resume control state untrailed.                                                                                                                                                                                                                            |
| codebase M2, architect M2, zigzag M1: new map allocation inside noexcept fold | Accepted | Add pre-mutation nonthrowing lookup preparation and planned LookupCapacityUnavailable refusal. New lookup commits do not allocate. Resource failure preserves logical state/fold position/counters; Store and advanceOrRefold stop with a planned typed exception rather than skip or return partial state. Existing semantic refusals remain distinct. Fault injection tests every new preparation allocation and retry/full-rebuild honesty. |
| vortex M1: Dewey branch-routing metadata omitted                              | Accepted | Keep explicit boundary metadata and currentDeweyPath behavior outside map-snapshot retirement. Differential gates cover path state, cut callbacks, scoped abort and spark metadata.                                                                                                                                                                                                                                                            |

## Source verification

- VM state is node-based today (`vortex_vm.hpp:872-884`); restoring an erased key with ordinary
  insertion can allocate despite reserved buckets. Existing abort restores whole snapshots
  (`vortex_vm.cpp:491-505`). The reversible adapter is a prerequisite, not merely later tuning.
- Resume writes/restores cellReferences around a local arena savepoint at `vortex_resume.cpp:62-99`.
  Comparison helpers accept concrete map/set types (`vortex_comparison.cpp:24-45,197-200`).
- `Manifold::applyStructure` is noexcept (`manifold.hpp:792-793`, `manifold.cpp:321-323`), advances
  foldedThrough before existing insertion paths, and may mutate content/lastOp before byRef inserts.
  Preflight must therefore move before those mutations. `FoldResult` currently lacks a resource
  refusal (`manifold.hpp:222-279`); v2 expressly plans that enum/API addition. Store currently
  ignores semantic refusals (`store.cpp:529-561`), so resource handling must be a separate branch.
- Dewey paths are derived on choice creation, restored on backtrack/scoped abort, and read by cut
  callbacks and OR-parallel export (`vortex_vm.cpp:1393-1433,1478,1528-1543,1575`,
  `or_parallel.cpp:224-258`). They are not disposable container snapshots.

No proposal approval is requested again: P1-P5 remain accepted. The error and adapter details are
within P1/P2's registered files. Round 2 reruns the five affected reviewers; writer and ergonomics
carry forward unless these changes materially alter their domains.
