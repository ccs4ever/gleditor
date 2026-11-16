# Round 4 disposition (performance-architecture-proposals.md)

Reviewed v4; revised the target to v5. Vortex converged. Ergonomics confirmed both round-3 findings
resolved, then identified missing presence tracking in P2. The affected architecture review
identified avoidable retention after root-choice eviction.

| Finding                                                          | Ruling   | Resolution in v5                                                                                                                                                                                                                                                                                                                    |
| ---------------------------------------------------------------- | -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| architect M1: dead owning trail prefixes survive root eviction   | Accepted | Destroy payloads below the minimum of all surviving choice and local-savepoint watermarks before returning from steal. Keep indices stable until safe compaction; rebase surviving choices when no local savepoints are active, before growth/budget refusal. Charge capacity retained while a local savepoint prevents compaction. |
| ergo M1: dense `byRef` cannot distinguish absence from slot zero | Accepted | Separate occupancy bitmap, optional API result, overflow-map lookup, and reset/rebuild rules. Price 4 bytes plus 1 presence bit per op, rounded to mapping pages.                                                                                                                                                                   |

## Verification

`Manifold::denseOf` returns an optional (`manifold.cpp:126-134`); slot construction uses
`slots.size()` before append (`manifold.cpp:477-492`), so zero is a valid dense index. A zero-filled
index array alone cannot represent absence. The bitmap is private lookup occupancy, not semantic
metadata moved out of ZigZag ranks.

P1's typed shared handles retain payloads independently of the arena. Dead-prefix destruction must
therefore be explicit; merely advancing a logical watermark is insufficient. Savepoint watermarks
must participate in reclamation even when no choices remain. No live undo state is evicted.

No new register IDs are needed: these corrections stay within P1 and P2. Round 5 reruns the affected
Vortex, ergonomics and architecture reviewers, with a codebase check of the dense lookup change.
