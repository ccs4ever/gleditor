# Round 4 critique: ZigZag Purist (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/2M/6m.

Verified landed: edge-taxonomy additions (pre-existing-member class, name-merge
map-seed class, residual rule, dim-before-map ordering); membership append under
name-merge sound (`rankTail(existingModule)` appends → posward-of-base → first-match
shadowing); `wholeSpace` confirmed exactly right (registry chains entirely on `+d.dims`
edges — `vortex_core.cpp:47–48,:24–27,:87–88`; `dimensions()`/`dimensionNamed` walk
that rank — lifting the `d.dims` discovery skip pulls it all); early-name mechanism
sound (primedia appends in emission order; `extraRoots` seeds `order` pre-BFS);
mark-scoped graft consistent (`release()` truncates; `opcodeMap_` untrailed →
post-`discard` registration; mark-time `isRegistrableOpcode` gate makes the re-decode
infallible); single-owner list landed; record walk roots landed; ~60 citations
spot-verified; honest positioning retained.

## MAJOR

### M1 — A rank member cannot anchor a private `+d.vars` rank: the holder is positional, and the ordering invariant is unstated and in tension with §4's mint order

Same defect the vortex and codebase critics found: `registerProgramImage`'s
`rankAfter(imageRecord, vars)` scans the shared-rank tail posward-first-match
(`vortex_vm.cpp:131–137`). The real vagent convention is an **unranked** holder:
`finalizeProgramImage` bare-`makeCell()`s `image` (`fragment.cpp:442`), `varsRecord`
tail-appends `"floor"`/`"ops"` onto `rankTail(image, vars)` (:443–457) — a private
rank; the `"imageRecord"` key reaches it via `+d.values` (`decls.cpp:2247–2250`;
registered `genesis.cpp:538`). The doc's one-cell `stdlib.image` on `home +d.vars` has
no nested rank — `"ops"` is a posward sibling on the shared rank alongside the
manifest's own member cells. Works only under two unstated invariants:

1. `"ops"` must sit posward of its own `stdlib.image` — §4's "minted at bootstrap
   start" vs. holder at "bootstrap end" is self-consistent only if the record's rank
   filing defers to the end; the natural misreading puts ops negward →
   `unexpected("carries no \"ops\" rank")` → the "absent → skip" arm silently registers
   nothing (`opcodeMap_` empty → write-guard map protects nothing; `getOpcodeKind`
   content-decode keeps ops running — the P15 protection contract silently absent).
2. The first-match window between holder and ops must contain no foreign `"ops"` —
   true today only via tail-append discipline; and a `stdlib.image` with no ops posward
   of another image's ops silently misregisters that enumeration.

**Fix:** adopt the actual vagent shape — `"stdlib.image"` key record `+d.values →` a
dedicated unranked holder whose private `+d.vars` holds `"ops"` (order-free, verbatim);
or keep one cell and state the positional contract + make a grafted holder's missing
ops a refusal. Note the sigCell shape already gets this right (a `+d.params` target
whose `+d.vars` is free → private `"in"`/`"out"` rank) — the asymmetry makes the
conflation look accidental.

### M2 — §5.2.3 edge classes are listed unordered; two pairs overlap such that wrong-order application recreates the hazards they prevent

- **Membership-append vs pre-existing-member.** `home +d.stdlib→localModule` with
  `map[localModule]=existingModule` satisfies *both* classes; applied as class 2,
  `link(rankTail(home,d.stdlib), d.stdlib, POS, existingModule)` severs the module
  mid-rank (`manifold.cpp:672–692`) — the exact R3-M2 hazard. Precedence must be
  stated: member graft-minted → tail-append; member pre-existing → skip-or-refuse.
- **Marked-Int64 vs OpHandle.** Kind-agnostic edge-existence detection sweeps OpHandle
  wing slots (which carry the same mark, required by promote's deferred path
  `arena_manifold.cpp:1812–1824`) into the Int64 arm — `asInt64` returns nothing on an
  OpHandle cell → literal reading refuses/misdecodes every winged op. Correct rule:
  *edge-existence detects ref-bearing; value-kind dispatches the decode*
  (Int64→`asInt64`→`map[]`; OpHandle→`handleTarget`→`map[]`).

## MINOR

m1. "skip when the edge already holds" — pin the query: it is *rank membership*, not a
    literal `src→dst` edge — `rankContains(anchor, member, dim)` traverses both
    directions (`manifold.cpp:697–736`); `linked(member, dim, NEG).has_value()` is the
    cheaper equivalent.
m2. Name-merge candidate set — pin to "image `home +d.stdlib` rank members whose full
    text `findModule()`s to an arena module"; the loose wording could seed-merge an
    unintended cell.
m3. "emitted early in op order" — name the mechanism: manifest cells in `extraRoots`
    seed `order` pre-BFS (`arena_manifold.cpp:1753–1759`), or an implementer will emit
    the manifest wherever the walk finds it.
m4. Grafted mark edges — state whether `+d.mesh.relocation` edges replay into the
    arena: replayed marks leave live relocation handles that a later user-document
    `promote` reads as ref declarations — name the choice.
m5. Phrasing — "the `"ops"` member inside `stdlib.image`'s vars" implies containment
    that does not exist under the one-cell shape; fix the phrasing or the shape (M1).
m6. Corroborations: `insertIntoRank` yields `anchor→cell→displaced` (order preserved);
    real hazards are `link()` onto occupied slot and per-member head-anchoring.
    `VortexCore::home_`/`dims_` are ctor-only — the arena-swap story must re-derive
    them or bound base identity (vortex m4 verified correct).

VERDICT: NOT CONVERGED
