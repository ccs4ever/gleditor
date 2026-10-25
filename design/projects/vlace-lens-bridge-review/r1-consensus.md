# R1 consensus matrix (all six critics, verbatim critiques on file)

Verdicts: all six NOT CONVERGED.

| # | finding | writer | vortex | codebase | architect | ergo | zigzag |
|---|---------|--------|--------|----------|-----------|------|--------|
| C1 | lensCommit vacuous — no op dirties cells; only Sql/Config lenses take writes | B | B | M | B | B | B |
| C2 | §5 example broken — find/foldMap/unit missing; Option-typed find ill-typed | B | — | M | B | B | — |
| C3 | meta: claim wrong — openLensUri has no meta branch; silent FilesystemLens fallthrough | — | — | B | — | M | m |
| C4 | no Proposal Register (rule 6) | — | — | B | — | — | — |
| C5 | "pure reads, ordering doesn't matter" false — refresh/mount mutate arena; lazy memoization | — | B | — | M | B | M |
| C6 | IO Cell/Bool/Int discard expected<string>; Cell 0 sentinel | — | M | — | m | B | M |
| C7 | session resolution by keyFor misses attribute cells | — | M | — | M | m | M |
| C8 | refresh() missing from mutating-op semantics | — | — | M | m | — | M |
| C9 | sessions_ unbounded; "forgetting releases" false — die with Compiler | — | m | — | M | m | m |
| C10| mount/unmount/next should return the delegate/page cell, not Bool | — | m | — | m | B | — |
| C11| setLink/makeText can write projected cells bypassing dirty_/provenance | — | — | — | — | — | B |
| C12| no file:line cites / no priced rulings (house style) | M | — | — | — | — | — |

Unique majors worth keeping: refOfWrapped doesn't force inner arg
(vortex M3); mark/trail regime vs shared arena (vortex M4); lensNext
stub-discovery story (ergo M1); `union:`/`log:` omitted from scheme list
(codebase m7); keyCell inverse doesn't exist — drop "round-trip"
(zigzag m); negward climb impossible — D1's "back" needs a retained
root (zigzag m); childrenAll mints dims (vortex m1); mount not limited
to router root (writer/zigzag); REPL :lens uses own-fold arena
disjoint from program cells (architect m7).
