# Round 4 critique: Vortex Expert (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/1M/5m.
Every r3-disposition item verified against code — all landed.

## MAJOR

### M1 — The `"ops"`-under-`stdlib.image` holder is *positional*: the ordering invariant is unstated and §4's mint order contradicts it

`registerProgramImage` walks `rankAfter(imageRecord, dims.vars)` — cells **posward** of
the argument on the *flat* `d.vars` chain — first-text-matching `"ops"`
(`vortex_vm.cpp:131–137`). There is no nested rank: a cell's `+d.vars` edge is its rank
adjacency. The vagent precedent gets true scoping because its holder is **unranked**:
`finalizeProgramImage` mints `image` fresh off any rank (`vagent/fragment.cpp:438–458`),
`varsRecord` tail-appends `"floor"`/`"ops"` onto `rankTail(image, vars)` — a private
rank, reached only via the `"imageRecord"` key-record's `+d.values` (`decls.cpp:
2247–2250`, consumed by `varsMember` `lineage.cpp:332–344` and `registerProgramImage`
`genesis.cpp:538`). `rankAfter(image, vars)` then enumerates *only* its own records.

The doc's `stdlib.image` is deliberately a `home +d.vars` member — consistent only under
an unstated ordering invariant: its `"ops"` record must be the first `"ops"`-texted cell
**posward** of the holder on the shared rank. §4 step 2 reads the opposite — ops minted
at bootstrap *start* (tail-appended → negward of the end-minted holder) →
`registerProgramImage` returns `unexpected("carries no \"ops\" rank")` → the "absent →
skip" arm registers *nothing*. Partially silent: `getOpcodeKind` content-decodes through
`kLastContentDecodableOpcode` (`vortex_vm.hpp:114,145–158`), so `RunCall`/`RunResume`/
`ScanPush`–`ScanSubject` (:123–142, registrable-not-decodable) stay unregistered and
undecodable, and `hasOpcodeKind`'s empty map disables the write guard.

Corollaries: "foreign `"ops"` records never scanned" holds only **negward** — a posward
foreign `"ops"` first-matches whenever a holder's own ops is absent/misordered (the §11
isolation gate plants only a negward foreign record); and "absent → no-op" isn't what
the API does — a holder with no own ops silently registers whatever `"ops"` is posward.

**Fix:** adopt the actual vagent shape — the `stdlib.image` record's `+d.values` head
cell is unranked and holds a **private** `+d.vars` rank containing the `"ops"` record —
`rankAfter` then enumerates only the private rank: true isolation, no ordering
invariant. (Or state the positional invariant: `stdlib.image` rank-linked *before* its
`"ops"` record, posward-adjacent — weaker, still correctable, but strictly worse than
the private rank.)

## MINOR

m1. §5.2.3's parenthetical misdescribes `insertIntoRank` — `insertIntoRank(cell, anchor,
    dim, POS)` yields `anchor→cell→displaced`, order *preserved*
    (`arena_manifold.cpp:1423–1439`). The real hazards: `link()` onto an occupied slot
    (severs) and per-member anchoring at the head (reverses accumulation). The
    tail-append advice is correct; the mechanism description isn't.
m2. `{std::once_flag; std::optional<Triple>}` can't be swapped as written — `once_flag`
    is immobile; the state must be held by indirection (`unique_ptr<State>` under the
    mutex) for `resetStdlibImageForTesting()` to swap it.
m3. "Each `VortexStdLib` records the arena it bound against" — `bindManifold`
    move-assigns `primary_.arena` *in place* (`vortex_host.cpp:155`), so `&arena()`
    never changes. The identity token needs specifying (`arena().base()`, a generation
    counter, or bound-`Manifold` identity) — or state the named call sites carry the
    invariant (`initHostServices` on `primary_.stdlib` and `vplEngine_.stdlib()` —
    verified reachable: `vpl_engine.cpp:77–86` builds `ownedStdlib_` over
    `primary_.core` with its own `ownedVm_`; accessor `vpl_engine.hpp:79–81`).
m4. Swap-clear taxonomy misses `VortexCore::home_`/`dims_` (19 `DimRef`s): `initGenesis`
    is ctor-only (`vortex_core.cpp:16`) — after a swap of a null-base arena they are
    dead refs of the same cleared class, and the graft seeds `map[imageHome]` from the
    arena home. Pre-existing (`bindManifold` has no in-tree callers today), but the doc
    claims the untrailed-state list is closed — bound it: swaps assume stable base
    identity, or re-derive `home_`/`dims_` on swap.
m5. Cite drift: `system_docs.hpp:261–265` → `kVortex*` at :261–266;
    `vortex_stdlib.cpp ~:2513,2531` → `run(cursor,100)` at :2511/:2529;
    `document_id.hpp` "random per `Store`" → random per `DocumentId`. All other checked
    cites resolve exactly.

## Verified-correct

- `wholeSpace`: discovery skip name-matches `d.dims`/`d.lineage`
  (`arena_manifold.cpp:1778–1782`); emission loop skips only `d.lineage` (:2074–2077) —
  lifting `d.dims` alone is exactly right for the 19-dim registry + `dims_.dims` +
  `d.params`/`d.mesh.relocation`.
- Swap clears: member names verified (`vortex_vm.hpp:303–306`, `ChoicePoint::mark`
  :163); `ChoiceStackLive` implementable — `release()` resize-*up* into fabricated
  `CellSlot`s on the new arena (:1671–1674) is the corruption.
- `expectedScrollBytes`: primedia appends at op-write time in mint order
  (`:1158–1167,:1993–1995`) — early op order does place names early; mechanism sound.
- `imageEpoch`: boundary constants at `vortex_vm.hpp:145–146`; no `OpcodeKind`→label
  table today (~100 `mintOpcode` literals); `structureVerbName` exists (`ops.cpp:30`).
- `clamp` signature claim exact (`:1604–1631`); `isCellRelocationHandle` OpHandle-gated
  in both overloads (`foreign_op_handle.cpp:100,111`); kind-agnostic detection is the
  right call.

VERDICT: NOT CONVERGED
