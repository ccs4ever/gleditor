# Round 7 disposition (performance-architecture-proposals.md)

The owner requested one independent C++ performance expert subagent pass over v6. Its verdict is
**NOT CONVERGED: 1 BLOCKING, 2 MAJOR, 3 MINOR**. The earlier six-role convergence is historical;
this additional pass reopens the design. The target remains v6 so these findings are reviewable
against the exact proposal reviewed. This round makes no runtime changes or new speedup claims.

## Verified findings

| Finding                                                             | Ruling         | Required follow-up; current status                                                                                                                                                                                             |
| ------------------------------------------------------------------- | -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| B1: P3 quote occurrences can acquire ExternRef values               | Accepted; OPEN | Retain occurrence exclusion inside the ExternRef branch before foreign delegation. Non-ExternRef slots may bypass it. Specify a regression case for an ExternRef-valued occurrence with differing local and foreign edges.     |
| M1: P1 shared-handle trailing can force repeated COW                | Accepted; OPEN | Specify boundary-aware first-write snapshots or a priced payload undo scheme, including nested choices/savepoints and lifecycle changes. Compare copied elements, allocations and retained payloads against current snapshots. |
| M2: P4 derives promised release savings from instrumented self time | Accepted; OPEN | Remove unconditional speedup/call-elimination claims. Compare ordinary release out-of-line, inline and LTO builds; record repeated timings, inlining evidence, text size and build/link costs.                                 |
| m1: wing calls are not allocation counts                            | Accepted; OPEN | Correct the empty-wing claim and measure arity, allocation and spill distributions before selecting inline capacity.                                                                                                           |
| m2: prefix compaction work is not amortized or priced               | Accepted; OPEN | Price linear moves/rebasing, distinguish reused capacity from released capacity, and measure repeated root-steal/growth. This is a measurement concern, not a proven regression.                                               |
| m3: dense index range does not imply dense useful mappings          | Accepted; OPEN | Measure text-heavy, branch-heavy and structure-heavy folds, including useful entries, initialization/page faults, reserved space and committed/RSS memory.                                                                     |

## Source verification

- `arena_manifold.cpp:544-556` excludes quote occurrences from proxies. `setValueBits` at
  `2135-2145` allows the caller's kind; `foreignOf` at `601-609` canonicalizes occurrences. The
  present `linked` local-only branch at `1632-1641` therefore cannot be removed solely because
  occurrence construction starts with None. Existing tests also mutate quote values
  (`tests/xuzz/arena_manifold_test.cpp:702`).
- `vortex_streams.cpp:243-250` copies an additionally shared stream; `349-353` copies a non-unique
  iterator. A retained undo handle changes uniqueness. Saving it after in-place mutation instead
  loses the old payload. The proposal must specify when each boundary acquires a snapshot.
- `segmented_ops_spool.cpp:397-408` performs bounds/base checks and pointer computation. Its
  instrumented self time is not a measurement of removable release call overhead. Section 6's
  remeasurement gate does not repair the contradictory P4 pricing claim.
- `vortex_core.cpp:394-397` returns an empty vector for an empty wing. `store.cpp:529-537` folds an
  ancestral path, so the whole spool's key range is not a useful-mapping density measurement.

The expert report preserves counterexamples, exact evidence and concrete fixes. No new benchmark
runs were necessary to establish the correctness counterexample or the specification gaps; proposed
performance outcomes remain unmeasured. P1 and P3 require design corrections before implementation
approval, and P4 requires an evidence-based cost/benefit statement.

## Applied revision for follow-up review

The owner subsequently requested applying these fixes and rerunning the expert to convergence. The
above OPEN status records round 7's v6 review; v7 now contains the following corrections for round 8
to verify.

| Finding | Applied design correction                                                                                                                                                                                                                                                                                                                                                             |
| ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| B1      | P3 retains quote exclusion inside the ExternRef branch, preserves base/local routing and names the ExternRef-occurrence regression cases. Skipped-lookup fraction is unmeasured.                                                                                                                                                                                                      |
| M1      | P1 snapshots mutable stream/iterator handles on first write per active rollback boundary before COW; nested boundaries have separate touched-key sets. Commit/cut retain undo records without allocating a parent-set merge. Rollback and root eviction have explicit tracking lifetimes. Pricing includes touched-set slots, capacity slack, boundary headers and retained versions. |
| M2      | Baseline and P4 text distinguish instrumented self time from release overhead. P4's claimed saving and unconditional call removal are deleted; text/I-cache/build/link costs and repeated release variant comparisons are required. Execution order is experimental and conditional.                                                                                                  |
| m1      | P5 distinguishes empty wings from allocating materialization. Eight is a provisional spill threshold; arity/allocation/spill histograms, smaller buffers and visitor/span comparisons are implementation gates.                                                                                                                                                                       |
| m2      | P1 prices compaction as linear moves/rebasing and distinguishes reusable slots from allocated capacity. Repeated-root-steal measurement is required; no amortization benefit is asserted.                                                                                                                                                                                             |
| m3      | P2 distinguishes key range from useful mappings and requires text/branch/structure-heavy folds, occupancy, initialization, page faults, committed/RSS memory and reserved virtual bytes.                                                                                                                                                                                              |

These are specification changes and future implementation checks, not newly measured improvements.
No C++ runtime code or fixture has changed in this revision.
