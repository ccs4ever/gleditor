# Round 2 critique: Vortex & Spool-Mesh Expert (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 5 MAJOR, 9 MINOR.

All R1 findings verified resolved; the marked-Int64 + harvest + restate + graft chain is
now specified end-to-end (`isCellRelocationHandle` is OpHandle-gated, so marked-Int64s
correctly ride the `lineage.cpp:189–249` harvest→`extraRoots`→restate path).

## MAJOR

### 1. (§5.2.3, P26) The marked-Int64 pass under-specifies the detection predicate and miss discipline

`isCellRelocationHandle` is **kind-gated** (`foreign_op_handle.cpp:100,111` —
`valueKindOf == OpHandle` required): an implementer reusing it detects no marked Int64 —
B1/B2 silently reborn. The correct predicate is kind-agnostic "has an outgoing
`d.mesh.relocation` edge" (`markedRelocationSlots` shape, `lineage.cpp:213–229`). And for
a marked Int64 a map-miss can *only* mean harvest/restate gap — inside/outside is
undecidable — so the discipline must be "always a named refusal, never verbatim".

### 2. (§5, §5.2.5, P30) `rebindInstance` under `Cpp`/`ImageThenCpp` fallback empties `routineBindings_` and errors `registerProgramImage`

`bootstrap()` populates `routineBindings_` inline (`vortex_stdlib.cpp:4002` et al.) and
emits **no** `+d.params`/`"ops"` records — those are generator-only per P28/P29. On the
Cpp path: `clear()` → rebuild-from-records → empty map → `clamp` wrong-arity (§2.2 reborn);
`registerProgramImage` → `unexpected("no ops rank")`.

**Fix:** emit `+d.params`/`"ops"` records **inside `build*Module`/bootstrap** so every
arena is self-describing and `rebindInstance` is uniform — or scope the record-driven
rebuild to grafted cells and keep bootstrap's direct populate.

### 3. (§5.2.5, P30) `opcodeMap_`/`cursorPC_` staleness on `bindManifold`'s in-place arena swap — same dead-ref class the doc fixes for `routineBindings_`

`primary_.arena = ArenaManifold(base)` move-assigns (`vortex_host.cpp:149–158`); the VM
survives with `opcodeMap_` keyed by dead refs — stale entries *alias* fresh cells (dense
indices restart) → `hasOpcodeKind` write-guard refuses innocent writes; `getOpcodeKind`
map-first returns wrong kinds. `cursorPC_` same hazard. Naive `clear()` drops
non-content-decodable registrations (`ScanPush`…, `vortex_vm.hpp:145–158`) and affects a
shared-VM second instance — spec must name both.

**Fix:** on arena swap, clear `opcodeMap_` and re-register the image manifest (runtime
mints self-register via `mintOpcode`); name the shared-`VortexVM` constraint.

### 4. (§5.2, §5.4) `graftImage` has no atomicity story — a mid-graft refusal leaves debris the next `ensureCells` grafts onto again

Mid-graft refusals (map-miss, member decode) can fire after cells/edges/text are
committed; nothing rolls back; a partial graft can even satisfy the `stdlibPresent`
probe → silent dead stdlib; or `ImageThenCpp` bootstraps *on top of* debris. Machinery
exists: `mark()`/`discard()`/`release()` (`arena_manifold.cpp:1596–1674`).

**Fix:** graft inside a taken `mark()`; `discard` on success, `release` on refusal;
registration/rebind run after `discard` (untrailed state); add a graft-refusal matrix row.

### 5. (§4 steps 5–6, P28/P29) Manifest-record mint ordering contradicts the cited restate precedent

The `seedProgramImage` order is records arena-side **pre-promote** (promoted, then
members restated through the twin map). Records minted post-promote never reach the
image. `stdlib.image` is the exception — `expectedScrollBytes`/`generatorHead` depend on
the finished output and have no ref members.

**Fix:** `"ops"` and `+d.params` records mint **arena-side during bootstrap** (restate
covers their members); `stdlib.image` mints store-side last.

## MINOR

1. `insertIntoRank(cell, anchor, dim, POS)` inserts posward-of-anchor — head-insert →
   same-text local symbol shadows base under first-match. Pin
   `link(rankTail(anchor,dim), dim, POS, member)` (the `exportSymbol` pattern).
2. (§5.1) `expectedScrollBytes` is a folded Int64 member — survives truncation; the check
   is post-fold, pre-epoch, not a "precheck before fold".
3. (§5.4) The deferred throw also fires inside `stdlibPresent`/`rebindInstance` resolves
   — gate it at public entry points; internal resolves bypass.
4. (§5) Diagram orders `rebindInstance` above `ensureCells` — backwards.
5. (§12) Restate SetValues (~3.8K) + marker links (~4K) unitemized → ~16–18K ops
   expected.
6. (§5.2.3) "wing ops-manifest members" conflates OpHandle slots with marked-Int64
   members.
7. `rawWingSlotsOf` has no callers but P11 consumes raw slots — name the slot decode
   contract.
8. Every `OpHandle→ephemeral` cell in the generator arena must carry the mark —
   one unmarked OpHandle hard-refuses the export (`arena_manifold.cpp:1812–1824`). State
   the invariant.
9. `candidatePreds` cite now `~:4887–4906`.

VERDICT: NOT CONVERGED
