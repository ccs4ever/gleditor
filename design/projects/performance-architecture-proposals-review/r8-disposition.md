# Round 8 disposition (performance-architecture-proposals.md)

The C++ performance expert reviewed v7 against all six round-7 findings and emitted **CONVERGED: 0
BLOCKING, 0 MAJOR, 0 MINOR**. No further design revision is required by this pass.

| Round-7 finding                                       | Final ruling | Evidence of resolution in v7                                                                                                                                                                                                                                  |
| ----------------------------------------------------- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| B1: mutable quote values invalidate P3 shortcut       | Resolved     | Non-ExternRef slots use the local scan; ExternRef slots retain occurrence exclusion before proxy resolution. Missing-dense/base delegation and quote-local edge/absence semantics are preserved; regression cases are specified.                              |
| M1: repeated shared-handle snapshots force COW        | Resolved     | Mutable stream/iterator handles are snapshotted once per key per active boundary before COW. Separate child touched sets and retained committed undo records preserve nested rollback without an allocating merge. Tracking and retained versions are priced. |
| M2: P4 promises release saving from instrumented time | Resolved     | Unsupported savings and call-elimination claims are removed. Release baseline/inline/LTO comparisons, inlining evidence, executable text and build/link resource costs are required before adoption.                                                          |
| m1: wing calls mistaken for allocations               | Resolved     | Empty wings are acknowledged; inline capacity is provisional and requires arity, allocation, spill and alternative-buffer measurements.                                                                                                                       |
| m2: compaction cost unpriced                          | Resolved     | Linear move/rebase work and retained capacity are explicit; repeated-root-steal measurement is an acceptance gate.                                                                                                                                            |
| m3: dense key range mistaken for useful density       | Resolved     | Text-heavy, branch-heavy and structure-heavy folds must measure useful occupancy, initialization/page faults, reserved virtual and physical memory before selecting the dense policy.                                                                         |

## Verified boundary cases

The expert checked the two parent/child commit cases. A previously touched parent retains its entry
snapshot; an untouched parent may record a post-child value on its first later write, while the
retained child record still restores the parent-entry value. Reverse LIFO replay reaches the right
state in both cases. Child rollback drops only child tracking/records; root eviction preserves all
surviving boundary watermarks and touched sets.

## Scope and checks

The requested fixes revise the design and its future implementation gates. They do not implement
P1-P5, run new benchmarks, or assert measured speedups. The C++ performance pass is complete; the
original six-role reviews remain historical records rather than new reviews of v7. The numbered
Proposal Register is ready for the owner's implementation decision, subject to its acceptance gates.

The proposal and all added review documents pass the required mdformat plugins, Markdown lint and
whitespace checks. No runtime tests were needed for these documentation changes.
