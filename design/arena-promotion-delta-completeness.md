# Overlay Promotion: Durable Edge-Clears and Value-Writes

**Document Version:** 0.4 (round-2 converged) **Status:** converged — edge-clear stage ratified and
ready to implement; value stage specified, gated on `Values`, and deferred (no consumer confirmed)\\

> **Panel outcome (two rounds, six lenses — CONVERGED):** edge-clears are ideologically and
> mechanically sound — the canonical "rearrange to limbo" DELETE — and the additive-only delta is
> the anomaly. Round 1 surfaced 4 blocking + ~14 major findings, all adjudicated into a
> `ShadowCommit` **flag-enum** gate (fixes the `function_ref` nullability compile error *and* splits
> edge- vs value-diffs), a `spanCount`/inference gate on the value stage (a `SetValue`+splice
> clobbers `(kind,bits)` on multi-span non-inferable cells), a hard-refusal cross-store guard, and
> tombstone **coexistence** (removal = structure, record = retired-marker). Round 2 verified every
> resolution against code — `e.pos != basePos` is an exact subsumption of the additive emission,
> `optional<function_ref>` compiles, `order ∩ shadowOrder_` is airtight — and returned zero
> blocking/major findings across all six lenses. §7 records each verdict.\
> **Related Documents:**

- [`arena-to-manifold-promotion.md`](arena-to-manifold-promotion.md) — §5.2 COW overlay delta
  promotion, the mechanism this proposal completes
- [`store-slice-convergence.md`](store-slice-convergence.md) — R6 (scalar duality), R8 (two manifold
  types / the ephemeral boundary)
- [`vagent-metaprogramming.md`](vagent-metaprogramming.md) — the consumer that exposed the gap
- [`vagent-metaprogramming-implementation.md`](vagent-metaprogramming-implementation.md) — the
  `unlearn:`/`fault:` tombstone workaround this proposal can retire

______________________________________________________________________

## 1. Why this document exists

`zigzag::promote()` is documented (and built) as the controlled crossing for speculative
`ArenaManifold` structure into a persistent `xanadu::Store`. For the **copy-on-write overlay** case
— an arena backed by a base `Manifold` — it is *delta* promotion: base cells map to themselves, only
invented cells mint, and modified edges become `SetLink` ops.

The delta is **incomplete**. Promotion commits only the *additive* half of an overlay's net change:

- **Dropped edges are silently lost.** An overlay that removes a base edge (`unlink` /
  `setLink(…, std::nullopt)`) produces no op — the base edge survives the fold as if the removal
  never happened.
- **In-place value writes on persistent cells are silently lost.** An overlay that overwrites a base
  cell's scalar bits or content produces no op — the base cell keeps its pre-overlay value.

The consequence surfaced in vagent metaprogramming: durable removal is impossible, so `unlearn` and
`d.code` de-registration had to retire structure with `unlearn:`/`fault:` **tombstone records**
rather than real removal — and a de-registered `d.code` head still resurrects at the next fold
because the unlink it performed was never committed. (Panel correction: the commit that actually
matters there is the `d.patch`/`d.code` **rank-tail unlink** — learned clauses are replay-minted
ephemerals, never `> imageExtent`, so their cell-unlink was never the durable path; and `prune`,
which operates on `≤ imageExtent` clauses and writes no tombstone, gains nothing from a
state-side-scoped gate. See §5.)

This proposal argues the fix is **not a rewrite** but a delta-completeness extension: the ops
vocabulary already expresses both missing cases, and `promote()` merely never emits them.

______________________________________________________________________

## 2. Established mechanics (verified against the code)

Every claim below was read out of the tree, not assumed.

### 2.1 The overlay shadow model

[`arena_manifold.hpp`](../apps/common/xanadu/zigzag/arena_manifold.hpp)

- `overlay_ : std::unordered_map<CellRef, std::uint32_t>` maps a **base `CellRef`** to the dense
  slot holding its shadow (L767). A shadowed cell **keeps its persistent `CellRef`** — no
  `ephemeralBit`.
- `shadowOrder_ : std::vector<CellRef>` (L770) records every base cell the arena shadowed, in order.
  It is the exact enumeration of "base cells this overlay touched," already maintained for
  `mark()`/`release()` and prefix export — and it is **already the documented `extraRoots` source**:
  `shadowOrder()` (L459–466) says a `promote()` call folding session mutations passes it "so a
  shadowed cell reachable only through an unshadowed base cell's persistent edges — which the root
  walk deliberately does not traverse — still folds." `vagent::lineage.cpp` **L197** already calls
  `promote(…, agentRoot, {}, durable)` with those durable state-side roots.
- `dimensionsOf(ref)` (`arena_manifold.cpp` **L956–981**): if `denseOf(ref)` resolves — the cell is
  shadowed — returns the arena's mutated edge set; otherwise falls through to
  `base_->dimensionsOf(ref)`. So for a shadowed persistent cell, `from.dimensionsOf(c)` is the
  *post-mutation* edge set and `from.base()->dimensionsOf(c)` is the *original* — the diff is
  computable directly.
- `unlink(from, dim, dir)` (`arena_manifold.hpp` **L625–628**) is exactly
  `link(from, dim, dir, std::nullopt)` — removal is represented as a null target, the same shape as
  the durable API.

### 2.2 The durable ops already express removal and restatement

[`store.hpp`](../apps/common/xanadu/store.hpp), [`store.cpp`](../apps/common/xanadu/store.cpp)

- `Store::setLink(parent, from, dim, dir, to, context)` takes **`to : std::optional<CellRef>`**
  (store.hpp **L599–605**). `to = std::nullopt` is the durable edge-clear: `Store::setLink` encodes
  `op.to = to.value_or(zigzag::noCell)` (store.cpp **L986**), and a `noCell` target folds as
  removal.
- This is not theoretical — `removePouchItem` already emits a durable unlink through the identical
  call: `setLink(curHead, *next, dimItems, DimVector::NEG, std::nullopt)` (store.cpp **L2808**).
- `Store::setValue(parent, cell, content, kind, bits)` (store.hpp **L653–660**) restates a cell's
  content span **and** typed value together — one op covers both the scalar-bits and
  content-overwrite cases.
- **No new op kind, no `CompactOpNode` layout change, no fixture regeneration.** Both needed verbs
  already exist on disk; `tests/samples/` fixtures are untouched.

### 2.3 Where `promote()` drops them

[`arena_manifold.cpp`](../apps/common/xanadu/zigzag/arena_manifold.cpp) **L2033–2066**, Phase 3:

```cpp
for (const CellRef arena : order) {
  if (!from.holdsOwn(arena) || from.isProxy(arena)) continue;      // L2034
  for (const auto &edge : from.dimensionsOf(arena)) {             // L2037
    if (noCell == edge.pos) continue;                             // live edges only
    const auto dim = real.find(edge.dim);
    const auto to  = real.find(edge.pos);
    if (dim == real.end() || to == real.end()) continue;
    if (nullptr != from.base() && !isEphemeral(arena) &&          // L2048
        !isEphemeral(edge.dim) && !isEphemeral(edge.pos)) {
      if (from.base()->linked(arena, edge.dim, DimVector::POS) == edge.pos)
        continue;                                                  // base already has it
    }
    out.version = store.setLink(out.version, real.at(arena), dim->second,
                                false, to->second);                // L2054 — additive only
    …known.advance(…)
  }
}
```

Two structural reasons removals and value-writes vanish:

1. **`dimensionsOf` yields only live edges.** A shadowed cell that dropped `C →dim→ X` to `noCell`
   simply has no posward edge on `dim`; the loop iterates the shadow's *current* edges and never
   looks at the base's. The dropped edge is never even *seen*, so no clear is ever emitted.
1. **Persistent shadowed cells map to themselves in Phase 2.**
   `if (!isEphemeral(arena)) { real.emplace(arena, arena); continue; }` (the overlay arm) skips all
   emission for the cell — so an in-place value/content overwrite on a shadowed base cell is never
   re-stated.

### 2.4 The one case already handled — re-points

Because ZigZag edges are **single-valued per `(cell, dim, direction)`**, a re-point `C →dim→ X`
becoming `C →dim→ Y` is already committed: the base-check at L2048–2052 sees
`base->linked == X ≠ Y`, does not skip, and `setLink(…, Y)` overwrites the old edge. The missing
work is narrower than "diff every base edge" — it is only the two cases where the *absence* of an
edge, or a changed *value*, is the change.

______________________________________________________________________

## 3. The design gap, restated precisely

Overlay `promote()` must emit the **net** change a reachable shadowed cell carries:

| base → shadow transition      | op today                      | op needed                       |
| ----------------------------- | ----------------------------- | ------------------------------- |
| edge added (`none → Y`)       | `setLink(…,Y)` ✓              | none                            |
| edge re-pointed (`X → Y`)     | `setLink(…,Y)` ✓              | none (single-edge overwrite)    |
| **edge dropped (`X → none`)** | **nothing** ✗                 | `setLink(…, std::nullopt)`      |
| **value/content restated**    | **nothing** ✗                 | `setValue(content, kind, bits)` |
| cell invented                 | `makeCell`/`makeScalarCell` ✓ | none                            |

The proposal fills only the two ✗ rows.

______________________________________________________________________

## 4. Proposal

### 4.0 The central decision: the diff must be opt-in, not default

The blast-radius audit (§4.6) found that **several existing callers depend on additive-only
promotion** — the gap this proposal fills is, for them, load-bearing:

- `vagent/p16.cpp` (**L89–141**) re-kinds *persistent* `d.refs` slots to `OpHandle` purely so the
  `isCellRelocationHandle` **discovery arm** pulls their ephemeral targets into `order` for minting;
  `restoreMarked` reverts afterward. Value-diffing those cells could persist an `OpHandle` value
  naming an ephemeral/unmapped target.
- `vortex/mesh_jobs.cpp` (**L319–323**) + `mesh_value.cpp` (**L168**) promote an arena overlaid on a
  **received remote base manifold** — but `base->store() == &store` there (promote targets the
  received store itself), so committing clears isn't "destruction into a remote store" so much as an
  *attributed lineage rewrite* in a local restored copy. The real hazard is that **evaluation-time
  shadowing isn't intent-to-remove**: incidental session writes would publish as durable removals.
  Re-points already carry this same capability today — a `to=0` op is a new *shape*, not a new
  hazard class — but the gate must still stay off, and the §4.5 cross-store guard **cannot** cover
  it (base==dest). It needs structural protection, not a convention.
- `vortex/vortex_stdlib.cpp` (**L5004, L5023**) promotes `core_.arena()` into a **different**
  destination store — the base-dedup compares the *source* store while emitting into the dest, and
  `setLink`/`setValue` resolve refs in the *destination* index space → a source-store ref that
  coincides with a valid dest index silently grafts the wrong cell.
- vagent `writeInt`/`rebindRefs`/`rebindDimRecords` shadow persistent slots **intentionally
  session-only** — genesis rebinds them every cycle; persisting them is at best redundant and, where
  the bits name a rehydrated position, actively corrupts the next genesis-(e) rehydration (the
  `OpHandle`-kind slot flipped to `Int64` hits the "verbatim" arm and stays dead — vagent B1).

So unconditional net-state diffing would silently change every one of these callers — and a
`bool(CellRef)` can't express what vagent actually needs ("edge-diffs for `c > imageExtent`, value
writes *never*"). The gate is an **optional per-cell predicate returning a flag set**:

```cpp
/// What a shadowed persistent cell may emit during promote, per cell.
/// Flags compose: a caller may allow clears but not values, or vice versa.
enum class ShadowCommit : std::uint8_t {
  Additive = 0,    // adds + repoints only (today's behavior)
  Clears   = 1<<0, // + edge clears (e.pos == noCell on a changed edge)
  Values   = 1<<1, // + value/content restatement (the deferred stage)
  Suppress = 1<<2, // emit NOTHING for this cell (not even adds)
};

struct PromotionBudget {
  std::uint32_t maxOps{65536};                 // reachable-cell bound (NOT an op count)
  /// Per-cell emission policy for shadowed persistent cells.
  /// std::nullopt (default) == legacy additive-everything, bit-identical to today.
  /// borrowed, not owned — call-scoped only.
  gleditor::cpp26::optional<
      gleditor::cpp26::function_ref<ShadowCommit(CellRef)>> commitShadowed{};
};
```

`optional<function_ref>` is required: `function_ref` has **no null state / no default ctor**
(`std23::function_ref` takes only `F*`/`F&&`; the codebase lens confirmed `commitShadowed{nullptr}`
cannot compile), so `nullopt` — not `nullptr` — is the "additive-only" spelling, and aggregate
designated-init `{.maxOps = N}` keeps working for every existing caller. The flag return resolves
the `bool` ambiguity outright: when the policy is **set**, it governs *all* emission for a shadowed
cell — `Additive`=adds/repoints only, `Clears`=+clears, `Values`=+value/content, `Suppress`=nothing.

`enum class` doesn't compose bits itself, so `ShadowCommit` gets the usual `|`/`&`/`~` overloads and
`policyHas(p, f)` reads `(p & f) != ShadowCommit::Additive`. `Suppress` is checked *first* — it wins
over any `Clears`/`Values` bits a caller also set (a cell marked `Suppress` emits nothing at all).
`Suppress` is an authorship-scope rule — it can't preserve the cell's *state* against reciprocal
severing: edges are single-valued, so a neighbor's committed op can still rewire a suppressed cell
fold-side (the `displacedUs`/`displacedIt` arms, manifold.cpp L509–517).

`regenesisCycle` then passes
`[E = imageExtent](CellRef c) { return c > E ? ShadowCommit::Clears : ShadowCommit::Additive; }` —
the *same* sideness test it already applies when seeding `extraRoots` (`lineage.cpp` L191–194), now
at emission too: state-side shadows commit removals, image-side ones keep additive behavior, and
`Suppress` remains available to a later verb that wants the image-side additive leak closed. (A
`commitStateSide(extent)` helper ships the common case so callers don't hand-roll the lambda —
footgun: `isEphemeral`/`isProxy`/`!holdsOwn` cells are never consulted, and `ephemeralBit`-set refs
are huge numbers that all read `> E`.)

### 4.1 The unified net-state edge diff (replaces the additive walk)

The unified loop still runs over **every `holdsOwn && !isProxy` cell in `order`** — the ephemeral
mints must keep emitting their links exactly as today. `policy()`/`base()` apply only to the
persistent-shadowed subset: for each `arena` in `order` that is a **persistent shadowed cell**
(`holdsOwn && !isProxy && !isEphemeral`), resolve `policy(arena)` —
`commitShadowed ? (*commitShadowed)(arena) : Additive`; for ephemeral or unshadowed cells `policy`
is `Additive` and `basePos` is `noCell` (`from.base() == nullptr` for base-less arenas such as
`importValue`'s). When `Suppress` isn't set, treat the shadow as the intended final state — sound
because a shadow is a whole-cell copy (`shadow()` copies the slot verbatim, L1481–1487), so
shadow-vs-base is a complete diff with no merge ambiguity. For each `DimLink e` in
`from.dimensionsOf(arena)`:

```cpp
const auto basePos = from.base()->linked(arena, e.dim, DimVector::POS).value_or(noCell);
if (e.pos != basePos && (e.pos != noCell || policyHas(policy, Clears))) {
  // Emit adds+repoints always (unless Suppress); clears only under the Clears flag.
  out.version = store.setLink(out.version, real.at(arena), realDim(e.dim), DimVector::POS,
                              e.pos == noCell ? std::nullopt : mappedTarget(e.pos));
  known.advance(store, out.version);
}
```

`e.pos != basePos` subsumes today's three arms — **add** (`base==noCell`), **repoint**
(`base≠shadow`), and **clear** (`shadow==noCell`, emitted only when `Clears` is set) — in one rule.
`mappedTarget`/`realDim` translate ephemeral refs through `real` and return *skip-on-miss* for
persistent refs absent from `order` (provenance cells, `d.dims` pos targets filtered at L1779);
persistent endpoints are otherwise identity-mapped via the `discover(edge.pos, true)` → mint-order →
identity-map path. A cleared base edge always survives in the shadow as a `DimLink{…, pos==noCell}`
(dead entries aren't removed mid-run), so the diff sees it.

**Negward edges emit nothing explicitly.** A severed negward edge survives as a cleared `pos` on the
*predecessor's* shadow — `link()` shadows displaced occupants (`arena_manifold.cpp` L1364–1373), so
the predecessor is shadowed and, if in `order`/`extraRoots` **and** `Clears`-opted, its own pos-diff
emits the clear. This preserves "only in-scope cells commit" and avoids half-severing an edge whose
far end the caller scoped out. **Consequence to document:** an inbound edge commits only if its
*source* is opted in — `zzDelete`/`unlink(C,dim,NEG)` clears C's side but leaves live durable
inbound edges from out-of-scope predecessors (consistent with the in-scope rule; fold-side, the
predecessor's `setLink` rewire still repairs the backlink, manifold.cpp L509–517).

**Deliberate redundant clears (M5):** `link()` shadows displaced occupants, so a repoint `P:X→Y`
also clears `Q`'s pos→Y edge on `Q`'s shadow (Q held the *new* target's neg side). `P`'s `setLink`
already clears `Q`'s end in the fold, and `Q`'s own pos-diff then emits a second
`setLink(Q,dim,POS,nullopt)` — redundant but idempotent (the second folds to identical state).
That's *by design*: each shadowed cell restating its own final truth is what makes the commit
convergence-independent; count it in the op-estimate.

### 4.2 The value/content diff (deferred stage — `Values` flag only)

This stage is **deferred** behind the `Values` flag and three correctness gates the panel surfaced.
vagent — the flagship consumer — does **not** opt into it (its session-only value writes are
load-bearing; see §4.0). It lands only after the gates below are in place.

For each shadowed `C` with `Values` set, compare shadow vs base on `(valueKindOf, valueBits)`
**and** the content-span run; emit nothing if identical (a shadow may carry a copied-but-identical
value, so compare content, not "is shadowed"). On a difference, gate then emit per `valueKind`:

- **Gate — the `SetValue`+splice clobber (codebase B2):** `SetValue` replaces the content run with
  *exactly one span* (manifold.cpp L553–557), and every subsequent `Splice` op **re-states
  `(kind,bits)` to `inferTextValue(assembledText)`** (store.cpp L902–924, fold `manifold.cpp`
  L536–540). So the gate keys on the **emitted sequence, not `spanCount`**: any emission containing
  a `Splice` requires `inferTextValue(assembledShadowText) == (shadow.kind, shadow.bits)` (the
  clobber is then a no-op — the last splice applies the same function to the same assembled text).
  `spanCount ≤ 1` suffices **only for a non-scratch single span** (setValue alone, no Splice); a
  *scratch* single span still routes through the re-inferring `Splice` fold, so it needs the
  infer-check too. Otherwise **skip with `GLEDITOR_LOG_WARN`** and document the residual — covers
  multi-span non-inferable cells (`OpHandle`, `ForeignOpHandle`, `Timestamp` with non-ISO text, a
  divergent `Int64`). (Same latent hole exists in today's mint path at L1984–1994 and the
  `cyclicHandles` arm at L2010–2015 — note it.)
- `Double`/`Bool`/`Int64`/`None`/`Timestamp`: `setValue(v, C, firstSpan, kind, bits)` then
  `spliceCell`/`spliceCellSpan` for `content[1..]` under the gate above. Prefer this over
  `Store::setCellText` — `setCellText` re-infers bits (store.cpp L953–959), which round-trips scalar
  kinds but not `Timestamp`/`ExternRef`-adjacent cases; exact restatement is the faithful choice.
- `scratchScroll` content: resolve via `from.scratchTextOf` and emit
  `setValue(v, C, {}, kind, bits)` + `spliceCell(v, C, 0, 0, text)` (permascroll append).
- **`OpHandle` — two guards, not one (codebase M2):** `valueBits` is a target `CellRef`. Ephemeral
  target → `real.at(t)` (skip+WARN if absent, matching the mint path's `seen.contains` shape
  L1816–1818). **Persistent** target → validate with `store.getCompactOp(t)` (the L1822 precedent) —
  the `real`-membership check alone doesn't cover persistent targets outside `order`, which would
  write a silently dangling handle. Additionally skip cells whose *base or shadow* kind is
  `OpHandle` unless the caller explicitly allows them — covers `p16`'s re-kind and genesis-(e)'s
  Int64-restore-over-OpHandle.
- `ForeignOpHandle`/`ExternRef`: skip — foreign-store payloads shouldn't be re-stated into this
  store.
- **Cross-store content (codebase M3):** a shadowed cell's content spans live in the base's scroll
  namespace; `setValue`/`spliceCellSpan` with a foreign span either trips `requireAddressable` or
  silently mis-addresses. If §4.5's guard ships as a hard refusal for non-null mismatched base
  stores this is unreachable — say so; otherwise the value stage needs the mint path's `canCarry`
  preflight (L1851–1866; `carrySpan` is the emit-time arm at L1931–1933).

**Ordering:** emit value diffs before link diffs — to match the mint path's "cell/value ops at mint,
links last" shape (L2023) — and `known.advance(store, out.version)` after *every* emitted op.
(`advance` is needed for the `manifoldAt`/`lastOpOnCell` fast path and early fold-refusal surfacing
— store.cpp L563–567 — not for chain correctness: `setLink`/`setValue`/`spliceCellSpan` chain
`op.source` via `lastOpOnCell`/`editedBirths_` independently of `known`, so interleaving is safe.)

### 4.3 The diff enumeration: `order ∩ shadowOrder_`

The diff runs over shadowed cells in `order` — correct and supported by the existing `extraRoots`
escape hatch:

- **A dropped edge `P →dim→ C` is cleared by diffing the source `P`**, which stays in `order`. The
  target `C` need not be reachable — its *inbound* edge lives on `P`.
- **`extraRoots = from.shadowOrder()` already pulls every shadowed base cell into `order`.** The
  walk deliberately skips non-ephemeral cells it doesn't own (L1768); `shadowOrder()` re-includes
  them. A caller folding *session mutations* passes it (`lineage.cpp` L197); a caller doing *answer
  extraction* gets only the reachable delta — also correct, an unreachable shadow is speculative.

`commitShadowed` then selects, *within* that set, which shadowed cells actually commit — the gate
that keeps `p16`, `mesh_jobs`, `vortex_stdlib`, and session-only slots untouched.

**Enumeration completeness — VERIFIED airtight (codebase lens).** Every overlay write path to a base
cell goes through `shadow()` (`link`/`unlink`/`setValueBits`/`setContent` all shadow before
touching, `arena_manifold.cpp` L1338–1377, L1300–1320; `setOneSide` is private and only invoked on
already-shadowed dense ids). So a dropped edge's source is *necessarily* shadowed — "unshadowed ⇒
nothing to commit" is a theorem, not a hope. **Caveat:** opt-in *without* `extraRoots=shadowOrder()`
diffs only reachable shadows — a caller wanting full session-mutation coverage must pass
`shadowOrder()` (as `regenesisCycle` does) *and* opt in, keeping the extraRoots filter and the
emission predicate coupled on the same sideness test.

**Stale-overlay lost-update (distributed lane):** the emitted `setLink(v, C, dim, nullopt)` chains
onto `lastOpOnCell(v, C)` — the *current* head of C's micro-history — while the diff compares
`from.base()->linked()` — a contractually immutable snapshot (arena_manifold.hpp L251–253). If the
destination store advanced between snapshot and promote, a clear lands on an edge the overlay never
saw. This is the *same* hazard additive re-points already carry (deterministic, name-chain-ordered);
accepted semantics: **"a clear wins over unseen newer base writes."** Document it; a future
conservative variant could compare `lastOpOnCell` state in the planning pass instead.

**`d.dims` caveat (Nelsonian lens):** the BFS skips `d.dims` edges in traversal (L1779), but the
shadow diff iterates `dimensionsOf` including `d.dims` DimLinks — so a caller unlinking a *dim* cell
could durably de-register a **dimension** (vocabulary removal, not just data). Legitimate under R12
(dimensions are cells), but `commitShadowed` predicates should decide consciously.

### 4.4 Budget & the planning pass: atomicity, not op-count

`maxOps` stays the **reachable-cell bound** (L1798, L1842–1844) — it has been cell-count since
introduction, and every consumer (`mesh_value` `limits.operations`, `vortex_stdlib` 100k/200k,
`vortex_host` 1000, tests) is calibrated to it. Repurposing it as an op-count silently tightens all
of them ~3× (mints + splices + links already exceed cell count) — a behavior break with no
compile-time signal. If an op bound is ever wanted, add a **separate `maxEmittedOps` field**.

The planning pass's real value is **atomicity**: pre-validate every `real`/`dim`/`OpHandle`
*membership* mapping before the first `out.version` write, preserving "a refusal leaves the store
exactly as it was" (L1869–1870). Caveat (codebase M4): `real` maps arena refs to *minted* `CellRef`s
assigned by `Store::apply` — unpredictable pre-write — so the pass validates that every required
ephemeral target/dim is **in `order`** (the `seen.contains` shape L1816–1818), not the minted value.
And the guarantee is *refuse-during-planning*, not atomic-emission: `setLink`/`setValue`/
`spliceCellSpan` throw `std::invalid_argument` after earlier ops land, with no rollback — the pass
should pre-check `lastOpOnCell(parent,c)`/`getCompactOp` for every persistent ref to eliminate the
new throw sites.

### 4.5 Cross-store base guard → hard refusal (and its limit)

`promote` should **refuse** when
`from.base() != nullptr && from.base()->store() != nullptr && from.base()->store() != &store` — not
warn. The `vortex_stdlib` hazard is worse than additive-only suggests: `setLink`/`setValue` resolve
refs in the *destination* index space, so a source-store ref coinciding with a valid dest index
**silently grafts the wrong cell** — a wrong-cell write, not just a wasted op. It exists today for
re-points; the diff widens it to every clear/restatement. Copy `lineage.cpp` L173–179's
null-tolerant shape (`Manifold::store()` is `nullptr` for hand-built base manifolds — null
tolerated, mismatch refused) and run it in the planning pass before the first write.

**The guard's limit — `mesh_jobs` is NOT covered.** There `base->store() == &store` (promote targets
the received store itself), so the guard never fires. Remote-base removal needs *structural*
protection: don't plumb `commitShadowed` into `publishValueImpl`/`mergeValues` at all (keep them on
a promote path without the knob), or assert `commitShadowed == std::nullopt` as a tripwire. This is
the one case where the gate must be enforced by construction, not convention.

### 4.6 Blast radius (verified caller audit)

| Caller                                                        | Base                                  | Additive-only load-bearing?                                                                                         |
| ------------------------------------------------------------- | ------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| `lineage.cpp:99` `seedProgramImage`                           | none (standalone compile arena)       | safe — no shadows                                                                                                   |
| `lineage.cpp:197` `regenesisCycle`                            | lineage fold                          | **intended primary consumer** (currently test-only — the mesh adapter seam isn't wired) — opts in `c > imageExtent` |
| `p16.cpp:89–141`                                              | lineage fold                          | **YES — relies on non-propagation**; edge stage needs no change, value stage skips `OpHandle` kinds                 |
| `writeInt`/`rebindRefs`/`rebindDimRecords`                    | shadowed persistent slots             | **YES — intentionally session-only** (genesis re-derives each cycle; persisting is redundant-to-corrupting)         |
| `mesh_jobs.cpp:319` / `mesh_value.cpp:168` `publishValueImpl` | **remote received manifold**          | **YES** — `base==dest` so §4.5 can't fire; needs structural off, not convention                                     |
| `mesh_value.cpp:538` `mergeValues`                            | none (base-less `importValue` arena)  | trivially safe — no shadowed persistent cells can exist                                                             |
| `vortex_stdlib.cpp:5004,5023` `export*ToStore`                | `core_.arena()` → **different store** | pre-existing hazard → §4.5 hard refusal                                                                             |
| `vortex_host.cpp:394,617`                                     | bound-store fold                      | additive anyway; §4.5 may fire (`bindManifold`/`bindStore` can diverge)                                             |
| `vortex_host.cpp:797,856`                                     | bound-store fold                      | same class as :617 — additive                                                                                       |
| `zzDelete` / `VortexCore::breakLink`/`writeValue`             | UI/VM ops on persistent cells         | **benefits** — becomes a real durable delete/write *once the host opts in* (none does today)                        |
| `vquery --in-place`, `sql_promotion`                          | coordinator / staging arena           | additive today; opt-in                                                                                              |
| `vlace` `IO.Promote`                                          | **not a promote call site**           | `compiler.cpp:1490-1499` is a store-less dry-run returning cell count — no opt-in needed                            |

**Dependents assuming additive-only (re-audit on opt-in):** genesis `replayPatch`'s `unlinkRank`
calls (genesis.cpp L281 alias-arm on `≤E` clauses; L341 `learn_method` unlink-first on `≤E` module
`d.vars`; **and** `dropPatch`'s `d.patch` unlink at genesis.cpp L222) start committing durably — the
*goal*, but the `fault:`/`unlearn:` read-arms must still read old tombstoned lineages;
`fileDimensionVocabulary` clears self-heal (re-mints missing names) but warrant a pin test; swarm
replication carries `to=0` ops unmodified. The full `unlinkRank` durable-commit surface under opt-in
spans `meta.cpp` L1487 (`graft_method`), L1504 (`prune`), L1722, L1880, L1914 (`dropDead`),
L1930/L1939, L1977/L1982, plus genesis.cpp L222 — *plus* the bare `arena.unlink` sites `meta.cpp`
L400, L406 (memo-record cleanup on `d.step` chains), L764 (cursor `restoreRank`), L1958 (`undoPatch`
clause detach): the diff is per-cell, not per-idiom, so these commit identically if their source is
shadowed/`>E`/in-scope.

______________________________________________________________________

## 5. Interaction with the tombstone workaround — **coexist** (unanimous panel ruling)

vagent currently works around additive-only promotion with `unlearn:`/`fault:` tombstone records.
The panel ruled **coexistence**, not retirement — removal and the record carry different semantics:

- **Removal owns structure; the record owns the retired-marker.** An edge-clear says "this structure
  is gone" (a fact); an `unlearn:` record says "this label is retired" (a predicate). A bare unlink
  can't carry the `module:name`-keyed suppression `symbolNamed` consults (meta.cpp L198–205), and a
  tombstone can't restore history truthfulness. Both ship, always.
- **Read arms stay forever.** A lineage committed under additive-only promote keeps its tail-dropped
  patches on `d.patch` (the unlink never committed); genesis-(d)'s `fault:`/`unlearn:` skip is the
  only thing keeping them dead. Real removal can't rewrite committed history — the arms are needed
  for backward compatibility regardless.
- **Write path stays too.** Cheap (one dedup'd `d.vars` record) and now means what it says:
  *retired*, distinct from *absent*. New capability it unlocks: a `relearn`/`forgive` verb can
  `unlinkRank` the `unlearn:` record itself and have *that* commit (`>E` rank) — impossible under
  tombstone-only.
- **Corrected scope (vagent B2):** the durable commit that matters is the `d.patch`/`d.code`
  rank-tail unlink. Learned clauses/symbols are replay-minted ephemerals whose unlink historically
  never committed regardless of sideness — the `d.patch`/`d.code` unlink is what gates re-minting.
  (Their committed twins can persist as `>E` zombies — the resurrection mechanism — and their unlink
  now commits under `Clears` too: a harmless durable-reclaim bonus.) `prune` gains nothing under a
  state-side gate for declared (`≤E`) operands — it has no sideness check and writes no tombstone.
- **Genesis replay re-audit:** `replayPatch`'s `unlinkRank` calls (genesis.cpp L281, L341) plus
  `dropPatch`'s `d.patch` unlink (L222) start committing durably — the goal. `fault:`/`unlearn:`
  read-arms still read old lineages correctly. The tombstone filter is consulted at three code sites
  (`symbolNamed` L198–205, `clausesOf` L214–217, the genesis arm L567–578 — which checks
  `fault:`/`unlearn:` records inline via `varsRecord`, not `unlearned()`) — not "four readers" as
  earlier drafts said.
- **`meta.hpp` L600–604 calls the tombstone the workaround it is:** "Promote never commits a
  removal, so an unlearned patch/clause/symbol physically resurrects on the next fold — this record,
  not the unlink, is the durable retirement." Once removal commits, that comment narrows to "the
  record is the retired-marker; the unlink now also commits."

______________________________________________________________________

## 6. Options considered

| option                               | what                                                                                                                                        | verdict                                                                                                                                                                                                                                                                                            |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **A. Tombstone-only (status quo)**   | Keep additive promote; `unlearn:`/`fault:` records stay the deletion channel.                                                               | Rejected — the suppression check is replicated across three readers, tombstones accrete forever on `d.vars`, and a lifted tombstone can't safely be un-lifted. (Panel note: tombstones *stay* as retired-markers — see §5 — they just stop being the removal mechanism.)                           |
| **B. Edge-clear diff** (land first)  | Pos-diff emitting `setLink(…, nullopt)` clears for `Clears`-opted shadowed cells; ~15 lines inside the link loop.                           | **Land first** — covers `unlearn`/`d.code`/`zzDelete` (rank-removals), zero format change, `p16` unaffected (re-kind is a value write). Session-only `writeInt`/`rebind` slots stay value-additive (`Clears` but never `Values`; no edge-drops). The whole need vagent actually has.               |
| **C. Value/content diff** (deferred) | `setValue` restatement for `Values`-opted cells, behind the `spanCount`/`infer` gate + `OpHandle` `getCompactOp` check + cross-store guard. | **Deferred** — gated on codebase B2 (multi-span clobber), M2 (persistent-target `getCompactOp`), M3 (foreign span carry), and vagent B1 (session-only value writes are load-bearing). vagent likely never opts in; the stage exists for `zzDelete`/`writeValue`-class durable writes where wanted. |

The `ShadowCommit` flag-enum (§4.0) lets one predicate express B-only (`Clears`), full diff
(`Clears|Values`), additive (`Additive`), or suppressed (`Suppress`) per cell — so B and C are the
same mechanism staged by flag, not two designs.

______________________________________________________________________

## 7. Panel verdicts (round 1 — six lenses)

1. **Nelsonian/ideological — RATIFY-WITH-CONDITIONS.** An edge-clear is the canonical "rearrange to
   limbo" DELETE (`OpKind::Delete` "stop pointing at content", ops.hpp L34; `to==0` ops.hpp L106),
   append-only and history-*producing* (fold inserts it into the cell's micro-history, manifold.cpp
   L524–525). The additive-only delta is the anomaly; the tombstone workaround is "provenance
   pollution." Conditions (pinning/docs): keep `commitShadowed` opt-in; pin provenance-exclusion +
   `d.dims` caveats with tests (C2, C4); document the source-side-clear provenance asymmetry (C3);
   tombstones stay distinct (→§5).
1. **Vortex codebase — 2 BLOCKING (resolved), 5 MAJOR.** Confirmed `order ∩ shadowOrder_` airtight
   (every write shadows the source), `e.pos != basePos` subsumption, OpHandle `real` translation.
   **B1:** `function_ref{nullptr}` won't compile → `optional<function_ref>` (adopted, §4.0). **B2:**
   `SetValue`+splice clobbers `(kind,bits)` on multi-span non-inferable cells → `spanCount`/`infer`
   gate (adopted, §4.2). Majors → `getCompactOp` persistent-target check (M2), foreign-span
   carry-or-refuse (M3), membership-validation rephrase (M4), deliberate redundant-clear note (M5).
1. **Distributed/systems — 0 BLOCKING, 2 MAJOR.** Confirmed deterministic replay (name-chain, not
   arrival), checkpoint/resume, swarm replication (all carry `to=0`), merge-order arbitration needs
   *no new rule*. **M:** §4.5 guard can't cover `mesh_jobs` (base==dest → structural off, §4.5);
   §4.5 must be a *hard refusal* for the cross-store identity-map hazard (adopted). Stale-overlay
   LWW documented (§4.3).
1. **vagent/PL-ergonomics — 2 BLOCKING (resolved), 3 MAJOR.** **B1:** a shared bool can't express
   "edges `>E`, values never" → `ShadowCommit` flag-enum (adopted, §4.0). **B2:** `unlearn`'s real
   commit is the `d.patch`/`d.code` rank unlink; `prune` gains nothing (corrected, §5). **M:**
   tombstones coexist (§5); `ShadowCommit` tri-state→flag (§4.0); leave `p16` re-kind alone —
   discovery-critical (§4.2).
1. **SWE/architecture — 0 BLOCKING, 3 MAJOR.** Confirmed no format/fixture change; `maxOps` stays
   cell-count, planning pass for atomicity (§4.4); `commitShadowed`/`ShadowCommitPolicy` endorsed
   (§4.0). Majors → pin `false`/`Suppress` semantics (§4.0); `regenesisCycle` is test-only —
   "intended consumer" (§4.6); `vlace` row corrected (§4.6).
1. **Technical-writer — 0 BLOCKING, 3 MAJOR, 12 MINOR.** Confirmed prose accuracy;
   `optional< function_ref>` fix (M1→§4.0); gate-semantics ambiguity (M2→§4.0 flag-enum); doc-update
   list — `docs/` doesn't exist, real targets are
   `vagent-metaprogramming-implementation.md:427–454`, `vagent-metaprogramming.md`,
   `store-slice-convergence.md:1908–1916`, `arena-to-manifold-promotion.md` §5.2+§7, ~10 stale code
   comments (§7.6). Minors → citation drift, glossary, perf/3-condition notes folded into §4.

**Round-1 outcome: edge-clear stage ratified for landing; value stage gated and deferred.** The four
blocking findings all resolved into the §4.0 flag-enum gate + §4.2 value gates.

## 7.6 Documentation updates required on landing

- `design/arena-to-manifold-promotion.md` §5.2 + §7 comparison table — document the diff +
  `ShadowCommit`/`commitShadowed`; the §7 "Mapped to self; zero ops" row (L352) needs a
  clears/values column.
- `design/vagent-metaprogramming-implementation.md` **L427–454** — the most directly contradicted
  passage ("a removed base edge is never un-emitted… only tombstone form works"); rewrite once
  edge-clears land.
- `design/vagent-metaprogramming.md` — verbs table (`unlearn`, `prune`, `d.code`), §6.6 durability,
  mint-and-repoint discipline.
- `design/store-slice-convergence.md` **L1908–1916** — "Not covered: promoting a shadow's content
  change back" caveat + `PromotionBudget` sketch (L337).
- **Stale code comments** (update with the feature): `arena_manifold.hpp` L188–189 (`maxOps` comment
  already wrong today), L232 (budget decl), L842–857 (`promote` contract); `meta.cpp` L355–358,
  L1984–1990, L214–216, L198–200; `genesis.cpp` L567–569; `records.hpp` L37–41; test comments
  `vagent_genesis_test.cpp` L1046, L1106, `vagent_plasticity_test.cpp` L1174–1228.
- `AGENTS.md` L330–333 — optional one-clause note ("overlay commits additions; clears/value-writes
  opt-in via `commitShadowed`").

## 8. Proposed test plan

Edge-clear stage (lands first):

- `ArenaManifoldTest.promotingAnOverlayCommitsADroppedEdge` — shadow a base cell, `unlink` an edge,
  opt-in `Clears`, promote, reload, assert the base edge is gone.
- `…promotingAnOverlayDoesNotDoubleEmitARepoint` — re-point an edge onto an *unoccupied* target (an
  occupied one triggers the designed-for redundant Q-clear, M5); exactly one `setLink`, no spurious
  clear on the same dim.
- `…commitShadowedNulloptKeepsAdditiveOnly` — same setup, `commitShadowed == std::nullopt`; assert
  byte-for-byte the current additive emission (the dropped edge survives).
- `…outOfScopeShadowIsNotCommitted` — shadowed cell not in `order`/`extraRoots` emits no clear.
- `…predicateSelectivity` — two shadowed cells, predicate admits one; assert only that one's diff
  commits (tests the gate, not order-membership).
- `…suppressEmitsNothingNotEvenAdds` — a `Suppress`-flagged cell emits no ops at all (suppression
  covers adds/repoints too, not just clears) — pins the flag's distinctive property.
- `…clearEmittedWhenSourceInScopeTargetOut` — dropped edge's source is opted-in but target isn't;
  the source-side clear still commits.
- `…netIdenticalShadowEmitsNothing` — drop + re-add the same edge, or a copied-but-identical value;
  assert zero ops emitted.
- `…unlinkRankPredSuccRewireCommits` — the real vagent idiom (records.hpp L41–52); assert rank reads
  correctly post-fold and `refusedOps() == 0`.
- `…crossStoreBaseIsRefused` — promote where `base()->store() != &store` → refuse (§4.5).
- `…commitShadowedCannotSeverProvenanceProjection` — a projected-authorship edge never reaches the
  diff (Nelsonian C2); and `…dDimsClearIsAPolicyChoice` (C4).
- `verifyAgainstFullRebuild` (manifold.cpp L1267) on the emitted op stream — the built-in honesty
  check.

Value stage (deferred — only if a caller opts into `Values`):

- `…promotingAnOverlayCommitsAnInPlaceValueWrite` — `setValueBits` on a persistent cell, promote,
  assert the new value persisted.
- `…opHandleValueTargetIsTranslatedThroughReal` — ephemeral target promotes naming the minted ref.
- `…opHandleValueWithUnmappedTargetIsSkipped` — `getCompactOp` guard + `GLEDITOR_LOG_WARN`.
- `…multiSpanNonInferableCellIsSkipped` — the §4.2 B2 gate pins the unexpressible case.
- `…scratchContentRestatedViaPermascroll`; `…foreignSpanIsNotCarried` (M3).

Policy / interaction:

- `mesh_jobs`: `publishValueImpl` against a remote base does not emit clears — **required** pin
  (§4.5 structural off).
- `p16`: re-kinded `d.refs` slots don't persist `OpHandle` values naming ephemeral targets.
- vagent: `d.code` de-registration no longer resurrects; `unlearn` on a new lineage commits the
  rank-drop; old tombstoned lineages still fold correctly; a `relearn` verb can clear `unlearn:`.

## 9. Resolved during drafting / remaining open

**Resolved while drafting:**

- ~~Reachability of a shadowed-but-unreachable base cell~~ — solved by the *existing* `extraRoots`
  parameter; `shadowOrder()` is its documented source (§2.1, §4.4). No new mechanism needed.

- ~~"Does any op express removal?"~~ — yes: `setLink(…, std::nullopt)` → `op.to = noCell` (store.cpp
  L986), already used by `removePouchItem` (store.cpp L2808). No format change.

- ~~Value restatement~~ — `setValue(content, kind, bits)` covers scalar + content together
  (store.hpp L653–660).

- ~~Fold/replay of `setLink(noCell)`~~ — **verified directly.** `Manifold::applyStructure`'s
  `SetLink` case (manifold.cpp L462–528): `to == noCell` passes the `UnknownTarget` check (L482),
  resolves to `target = std::nullopt` (L496), clears the displaced neighbor's backlink
  (`setOneSide(*other, dim, -dir, noCell)`, L509–517), then clears our side
  (`setOneSide(d, dim, dir, to)`, L522) with no new backlink (L519–521). Because every peer and
  every checkpoint restore folds the *same* `CompactOpNode` stream through this one code path, a
  clear replays identically everywhere — replication/resume need no special handling.

- ~~`SetValue` restatement~~ — `setValue(content, kind, bits)` covers scalar + content together
  (store.hpp L653–660); the `SetValue` fold case is adjacent at manifold.cpp L545.

- ~~`exportPrefix`/`hydratePrefix` round-trip of `shadowOrder_`/`overlay_`~~ — **verified.**
  `exportPrefix` writes `out.shadowOrder` (arena_manifold.cpp L2100); `hydratePrefix` restores it
  (L2117) and rebuilds `overlay_` by scanning `slots_[dense].birthOp` for non-ephemeral refs
  (L2123–2127). That scan is correct *because* `shadow()` leaves `birthOp` as the base cell's
  persistent ref — "the shadow's name is the base cell's ref and not refOf(dense)" (L1489–1490) — so
  every shadowed cell re-registers `overlay_[baseRef] = dense`. A checkpointed overlay still
  diff-promotes.

**Resolved by the round-1 panel:**

- ~~`maxOps` op-count vs cell-count~~ — **cell-count** (SWE ruling; repurposing silently tightens
  every caller ~3×). Op bound, if ever wanted, is a separate `maxEmittedOps` field. §4.4.
- ~~tombstone retire vs coexist~~ — **coexist** (unanimous): removal = structure, record =
  retired-marker; read arms stay forever, `relearn` becomes expressible. §5.
- ~~`commitShadowed` semantics/compile~~ — `optional<function_ref<ShadowCommit(CellRef)>>` flag
  enum; `nullopt` = additive-everything; per-cell `{Additive, Clears, Values, Suppress}`. §4.0.
- ~~merge-order arbitration for removals~~ — **none needed** (chain-LWW + branch-isolation +
  signed-inventory); clears can't materialize concurrently on the same edge at merge.
- ~~remote-base removal legitimacy~~ — **structural off** for `mesh_jobs` (base==dest; §4.5 can't
  fire); evaluation-time shadowing isn't intent-to-remove.
- ~~`p16` re-kind fix~~ — **leave alone** (discovery-critical); value stage skips `OpHandle`-kind.
- ~~`vlace`/`graft_*` call sites~~ — audited (§4.6); `IO.Promote` is a store-less dry-run, no opt-in
  needed.

**Remaining open for round-2 convergence:**

- The `Values` stage's three gates (§4.2) are *specified* but deferred — needs a value-stage
  consumer before landing; none confirmed yet (vagent opts out).
- `Suppress` semantics for the image-side additive leak: the gate exists, but whether a verb wants
  `c ≤ imageExtent → Suppress` is a later PL-semantics decision.
- The mint-path's *latent* multi-span `OpHandle`/scalar clobber (§4.2 note) is pre-existing and out
  of scope — flag for a separate fix.

______________________________________________________________________

*v0.4 — **CONVERGED.** Round-2: all six lenses (Nelsonian, codebase, distributed, vagent, SWE,
technical-writer) signed off with zero blocking/major findings; `e.pos != basePos` verified an exact
subsumption of the additive emission, `optional<function_ref>` confirmed to compile,
`order ∩ shadowOrder_` airtight, and the distributed invariants re-checked end-to-end. ~20 minor
cite/wording slips across both rounds folded in. **Edge-clear stage is ratified and ready to
implement; the value stage is specified, gated on `Values`, and deferred (no consumer confirmed).***
