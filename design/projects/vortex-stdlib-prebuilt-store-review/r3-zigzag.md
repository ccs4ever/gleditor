# Round 3 critique: ZigZag Purist (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/2M/5m.

## Verified clean

- Per-ref markers adopted correctly; no residual shared-marker language (only the
  impossibility argument at §4/P26). Positional members verified topologically sound —
  `seedProgramImage` precedent confirmed verbatim (`lineage.cpp:329–345,390–415`);
  `registerProgramImage` consumes the identical shape (`vortex_vm.cpp:121–155`).
- Neg-side edges: `link()` writes both half-edges in one op (`manifold.cpp:689–692`) —
  "replayed once, pos-side" is closed.
- `rankTail` exists and is total (`cell_views.hpp:258–262`); tail-append =
  `exportSymbol`/`getOrCreateModule` verbatim (`vortex_core.cpp:117–119,128–130`);
  `insertIntoRank` head-insert confirmed (`arena_manifold.cpp:1423–1439`);
  `findOrMintDimension` appends internally (`:87–88,99–102`).
- Fold-birth fiat: `home_`/`dimsDim_` = first two births (`manifold.cpp:447–454`); promote
  emits `order{root}` then extraRoots (`arena_manifold.cpp:1745–1759`).
- Unscoped records: `owns` lambda unscoped-matches-any (`records.hpp:103–109`); scope arm
  always adds `+d.owner` (:114–118) — real distinct generator path.
- Honest positioning retained (§5.2 named compromise verbatim; U-X2/U-X5/U-X7 deferred).
- `zzResolveDim` confirmed fixed — walks `rank(dims().dims, dims().dims)`
  (`vortex_stdlib.cpp:5959–5974`); `exportSymbol(fail)` `:4009`; `buildGoalModule` anchors;
  `zigzag/sql_lens.cpp:703`; `isCellRelocationHandle` OpHandle-gated
  (`foreign_op_handle.cpp:100,111`); 19 named dims (`vortex_core.cpp:52–73`).
- Mark-scoped graft rollback real (`release()` truncates `slots_`/`links_`/`content_`,
  `arena_manifold.cpp:1657–1674`); `outstandingMarks()` public (`arena_manifold.hpp:
  750–752`); all second-instance sites verified.

## MAJOR

### M1 — §5.2.4/P28/§6.4: `registerProgramImage(map[home])` cannot deliver record-scoped registration (first-text-match scan)

`registerProgramImage` first-text-matches `"ops"` under `holder +d.vars`
(`vortex_vm.cpp:131–137`, `break`); `map[home]`'s `+d.vars` is the shared rank. Grafted
records tail-append, so: base's record shadows the local image's (§6.4 registration call
re-registers base, never consumes local enumeration); a foreign `"ops"` record posward of
the grafted one is first-matched → misregister or non-atomic abort mid-walk
(`vortex_vm.cpp:149–153`); and `rebindInstance` for a non-grafting second instance faces
the same record-identity problem — scoping needs record identity per arena, not per
graft. Aggravating: "minted at the end of each `build*Module`" reads as 21 same-text
records — first-match would register only the earliest module's ops even in a virgin
arena. Fix: the graft knows `map[opsRecord]` by identity — collect translated members
during step 3 and feed `registerProgramOps` (P28's own "accepted as equivalent" fallback),
or add a record-cell overload; pin cardinality (one record vs per-module). Note
unregistered ops still run via `getOpcodeKind` content decode (`vortex_vm.cpp:186–196`) —
the loss is the write-guard's map-only protection plus the contract.

### M2 — §5.2.3/§6.4: edge taxonomy not closed — member endpoint mapping to a pre-existing cell severs a populated rank

Missing class: **dst maps to a pre-existing (non-grafted) cell.** §6.4's module
name-merge puts `map[localModule] = existingModule`; the local `home +d.stdlib→
localModule` edge then has its member already on the rank. Under the stated rules it
tail-appends: `link(rankTail,d.stdlib,POS,existingModule)` — `displacedIt` = its mid-rank
predecessor P → P's `+d.stdlib` cleared (rank severed at P) and the returned chain rings
(`manifold.cpp:667–692`). `insertIntoRank` doesn't save it — `rankContains` walks
posward *from the passed anchor* (the tail) so a mid-rank member escapes detection. Same
class: (b) grafted-src → pre-existing-dst on a non-registry dim — unclassified; (c) dim
classification order vs `map[]` consultation unpinned — an unfiled non-`"d."`-texted
edge-dim is a cloned cell with a valid map entry, so consulting `map[dim]` first
recreates the §2.3.7 defect instead of reaching refusal; (d) name-merged module cells are
a third map-seed class §5.2.1 doesn't name (and the intended-subgraph definition misses).
Fix: (1) explicit class — edge whose non-anchor endpoint maps to a pre-existing cell:
skip when the edge already holds in the arena, otherwise named refusal; name-merged
content cells join the map at seed time and are excluded from cloning; (2) residual rule —
any remaining edge touching a pre-existing cell → named refusal — closes the taxonomy;
(3) pin dim classification before `map[]` consultation.

## MINOR

m1. §5.2.3 "the invariant below" dangles — single-owner is stated only in §4 (above);
    restate in §5.2 or point upward.
m2. §4 single-owner list omits `±d.grab` (wing heads — the largest structure class) and
    `d.mesh.relocation` mark edges; add or rephrase "every structural dim's ±adjacency."
m3. P27 Location `~:5502` vs §2.3 `~:5674` — actual `:5674`; fix P27.
m4. `expectedScrollBytes` degrades on truncation: the record is found by key text and the
    text dies first at the scroll tail (clamps `""`, `segmented_primedia_spool.cpp:
    129–132`); the Int64 bits survive but the record can't be located — the refusal
    degrades to "manifest missing." Still named (goal holds) but the scroll-specific
    cause is lost in the case the ordering targets; note the degradation or add a
    text-independent fallback.
m5. Record-member walk roots unpinned — name them: grafted `"ops"` records under
    `map[home] +d.vars` and grafted sigCells' `"in"`/`"out"` via each grafted sym's
    `+d.params` edge (record identity by minted-set membership, not text).

VERDICT: NOT CONVERGED
