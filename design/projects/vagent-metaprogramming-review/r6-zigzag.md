# R6 — Xanadu / ZigZag topology critique (v1.5)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the r5 disposition.

Verified against `apps/common/xanadu/zigzag/arena_manifold.cpp` (`link` reciprocal eviction
1347–1377; `rankContains`/`insertIntoRank` no-op 1381–1439; `promote` BFS 1744–1788, untouched-base
endpoint rule 1755–1757, relocation pull-in 1759–1763, `OpHandle` gate 1797–1809, Int64 verbatim
1934–1936, `real.at(target)`/verbatim-persistent arm 1947–1951, `d.dims` skip 1766; `birthOp =
refOf(dense)` 1178–1187; shadow keeps base ref/`birthOp` 1481–1503; `ensureDimension` name-dedup
78–103), `arena_manifold.hpp` (`refOf = ephemeralBit|dense` 468–470, `insertIntoRank` idempotence
contract 624–628, overlay/shadow doc 20–47), `manifold.hpp` (`ephemeralBit`/`isEphemeral` 154–157,
**`birthOp` doc "== this CellRef" — persistent refs are dense monotone mint indices** 295),
`cell_views.hpp:258` (`rankTail`), `mesh_value.cpp` (`importValue` fresh base-less arena 416,
all-ephemeral twins 481–492, unmarked `OpHandle`→`None` 485–490, marked→`OpHandle` naming *local*
twin 494–501, relocation-target identity check 396–400), `mesh_actor.cpp:270-289` (`WorkerContext`
overlays the checkpoint `Manifold`, `stateRoot = checkpoint->root`), `vortex_core.cpp` (`+d.grab`
input-wing/`+d.step` chain convention 386–395, `exportSymbol` `sym +d.values → entryOp` 119–126,
`+d.name → nameCell` 480–485), `vortex_stdlib.cpp:4238-4241` (`addClause`: `rankTail(predCell,
d.clause)` — per-predicate clause rank).

## R5 verdicts (zigzag-1..6)

| r5 finding | verdict |
|---|---|
| 1 MAJOR — `birthOp` window domain unstated | **Resolved — adopted verbatim and mechanically verified.** :1296-1308 replaces the timestamp framing with exactly the proposed discriminator: "a **persistent (non-`ephemeralBit`) ref at or below the recorded program-image extent** … ref-space ordering, not a timestamp", recorded once by the loader, bound at genesis (c) (:1524-1528), never re-marked per genesis (:1299-1300 — correctly forestalls the per-incarnation absorb-everything failure). Mechanics confirm: persistent `CellRef`s are dense mint indices (`manifold.hpp:295` — `birthOp == this CellRef`; `manifold.cpp:434` `birthOp = opIndex`), so `≤ extent` is a real ordering; `ephemeralBit` is the top bit (:154-157) so every arena mint sorts above any persistent boundary; a shadow's ref *is* the base ref (`overlay_.emplace(ref, dense)`, :1501-1503, doc hpp:43-46) so shadow-of-program classifies program-side and shadow-of-promoted-state stays state-side — both as v1.5 claims (:1305-1308). Residual specification gap → new finding 1 (MINOR). |
| 2 MAJOR — no transit restriction | **Resolved — adopted verbatim, beneficiary list matches my fix.** :1308-1315: "link-reachable *from the agent cell* — in either direction, as `promote` walks — **without transiting a program-side cell** (the walk prunes at the program-side boundary)". The named exclusions (memo records behind clause `+d.code`, wrappers, installed `learn` copies behind `d.clause`, frames behind `home`'s `+d.cursors`/`+d.stack`) are each mechanically correct — every one anchors on a program-side cell. Consistent with `promote`'s own boundary (:1755-1757 — non-ephemeral cells the arena does not `holdsOwn` are endpoints, never expanded). No circularity: program-side is computed by its own outbound walk + ref arm before the state walk runs. `closed`'s arm uses "the same test" (:1310-1311, :1382-1384) and the frame-cell case invokes it explicitly (:1392-1393). Residual nits → findings 2, 5. |
| 3 MINOR — memo `+d.name → key` eviction | **Resolved in full.** :286-292 (mirrored :1337-1339): records are `{+d.values → wrapper, +d.name → memo-kind label, +d.grab → fresh scalar key cell}`; the key is "**content compared by value, never a link to the attach cell**, because the attach cell's single `-d.name` slot could hold only one memo's key edge" — the eviction is now named and avoided by construction. The wrapper `−d.values` caveat I asked for is stated (:290-292: "safe because a builtin-minted wrapper carries no emission-record anchor"). `+d.grab` occupancy verified safe — see new-findings preamble. Carry nit → finding 3. |
| 4 MINOR — dim vocabulary/d.dims | **Resolved.** :1292-1296 adds `d.extends` (real program-side dim, `vagent-agent-language.md:481`) and names `d.dims` "the one named exclusion, matching the checkpoint rule (`promote` skips `d.dims` edges, `arena_manifold.cpp:1766`, and `ensureDimension` re-mints dim cells at genesis)". The dup-dim corner closes mechanically: `ensureDimension` dedups by name (`dimensionNamed` first, :78-80), and a promoted dim cell is an orphan text cell — never on a `d.dims` rank, never in `arenaDims_` — so `dimensionNamed` never resolves to it. The remaining runtime dims (`d.cursors`/`d.stack`/`d.cache`/…) are covered by the transit clause rather than the vocabulary: cells behind `home`'s cursor/stack ranks are excluded via the program-side `home` anchor, matching :1313-1314's claim. |
| 5 MINOR — `reify:` spelling | **Resolved.** Canonical `reify:<src>:<serial>` at :1050, :1571, :2308, :2384; the `reify:` mentions at :1329/:1568 are the serial noun-phrase, not label spellings. |
| 6 NIT — `d.clause` attribution | **Resolved.** :1355-1357 now reads "`d.code`, `d.patch`, `d.synapse` on the agent cell, and each goal's/event type's `d.clause`" — matching `addClause`'s per-predicate `rankTail(predCell, clause)` (:4238-4241). `rankTail` correctly cited to `cell_views.hpp:258`; `insertIntoRank` same-dim splice at `arena_manifold.cpp:1423-1439` verified. |

## Direct answers to the round's three scoped questions

**Does "persistent ref ≤ recorded extent" misclassify `importValue` cells? — No.**
`importValue` builds a fresh `ArenaManifold` with *no base* (`mesh_value.cpp:416`) and mints every
local twin by `arena->makeCell()` (:481, :492) — every imported cell is `ephemeralBit|dense`,
including relocation-handle retargets (`local.at(*target)`, :494-501). No imported cell can carry a
persistent ref, so none can satisfy the extent arm; imported graphs classify purely by reachability,
which is the correct outcome (a received value is runtime state). The adjacent real gap is the
*program image's* own ingress — finding 1.

**Does the memo `+d.grab` key cell collide with list-convention occupancy? — No.**
`+d.grab` heads an op's *input wing* whose items chain on `+d.step` (`vortex_core.cpp:386-395`); the
only readers anchor on op cells and step from the *first grab item* (`rankTail(*first, step)`,
:393-394) — a record's `+d.grab` is inert because records are never wing-anchored, and the record's
own `+d.step` memo-chain membership is a different dim slot, never read as wing continuation (wing
traversal starts at `linked(anchor, grab)`, not `linked(anchor, step)`). The fresh key cell's
`−d.grab` is sole-occupied. The remaining exposure is the *label* cell, not the key — finding 3.

**Does the non-transit clause break the state-side walk's reachability claim? — No — sound, one
root nit.** The classification is acyclic (program-side needs only the `program()` walk + ref arm),
the prune composes correctly with bidirectionality, and every cell the doc wants dead (memos,
wrappers, installed copies, frames, `d.validate` tags) verifiably sits behind a program-side anchor
while everything it wants durable (`d.code`/`d.patch`/`d.vars`/`d.synapse` on the agent) hangs off
the agent's own ranks with no transit. The durable set escaping through `promote`'s *different*
boundary (`holdsOwn` shadows are expanded) is sealed by the `d.code` no-program-edges invariant +
`closed` re-verification, as the doc intends. The residual is the walk's *root* — finding 2.

## New findings (v1.5 deltas only)

### 1. MINOR — the extent arm silently presumes one persistent numbering; "(c) re-import the prelude" names the one ingress that would break it

The test "persistent ref ≤ recorded extent" is only meaningful if the program image and the durable
state share a single `CellRef` numbering. v1.5 gestures at this — "the overlay already holds the
promoted cells" (:1300, :1527) — but never states the requirement, and the checkpoint manifold is
the arena's *only* base (`WorkerContext(checkpoint->manifold)`, `mesh_actor.cpp:270-271`). Two
available-but-wrong ingresses both collapse the program-side set to empty:

- **`importValue` as the program image's ingress**: "re-import the prelude" (:1522) read as the
  codebase's import verb produces all-ephemeral twins (above) → no cell satisfies the persistent arm
  → the program-side set is empty → the transit prune never fires → every agent-reachable skeleton
  cell is *state-side* → `promote` drags the whole skeleton into every checkpoint — the V12 failure
  re-entering through the state-side door this revision just built.
- **Program image in a sibling restored `Manifold`**: persistent refs are dense indices per manifold
  — ref 42 in the program manifold and ref 42 in the checkpoint manifold are different cells; `≤
  extent` across numberings is garbage either direction.

**Fix.** One clause: "the program image is restored ref-preserved into the same numbering the
checkpoint overlay carries — its cells keep refs ≤ the recorded extent; `importValue`'s remapped
ephemeral twins and any sibling-manifold ingress would empty the program-side set, so `importValue`
must never be the image's path (contrast the resume rule already stated, :1532-1535)."

### 2. MINOR — the state-side walk's root exemption is unstated: a program-side `agent_cell()` prunes the entire durable set

"Link-reachable from the agent cell without transiting a program-side cell" (:1308-1310) never says
the root is exempt from the prune. `agent_cell()` (:998) is the anchor of every durable rank; if the
running agent's cell were ever a program-image cell — a `agent Forager` declaration minted
pre-extent and used as the instance cell — it classifies program-side, the walk prunes at its own
root, and the durable set is empty (no `d.code`, no `d.patch`, no `d.vars` — silent total loss of
durability). The design plainly intends the instance cell to be runtime-minted at spawn (ephemeral)
and promoted post-extent thereafter — both safely non-program-side — but that fact is load-bearing
and unwritten. One clause: "the walk's root is the running agent's cell, which is never a
program-image cell (spawn-minted, promoted post-extent) — the prune cannot fire at the root."

### 3. NIT — memo record's `+d.name` label cell needs the same freshness the key just got; the "identifying label" is undefined for bare-op attach cells

`:287-290` fixed the key (`+d.grab →` fresh scalar, content-compared) but leaves "`+d.name →` the
memo-kind label ("test"/"body")" unquantified: if the two memo kinds share an interned label cell,
the second record's `+d.name` edge evicts the first's `−d.name` (reciprocal eviction, :1347-1377) —
the same trap class the fix escaped on the key side. Say the label is a per-record fresh text cell
(the `makeCell(name)` precedent, `vortex_stdlib.cpp:4053`). Separately, the key is "the attach
cell's identifying label" — but a clause `+d.grab`/`+d.spin` may hold a *bare op* directly
(:986-987), which carries no `+d.name` label; the memo then has no computable key. One half-clause
("a label-less attach cell never memo-hits" or fall back to a minted serial).

### 4. NIT — label-vs-qualified shape test needs an order: `src`/`reify`/`patch` are legal module names

The rebind dispatch is "by shape" (:1431-1443) and first-`:` splitting fires "only when [the prefix]
resolves as a module on `programRef`'s walk" (:1574-1577) — but nothing bans a module literally
named `src`, `reify`, or `patch`. If one exists and resolves, a patch target or record name
spelled `src:demo:12:4` splits as module `src` + name `demo:12:4`, misses `symbol_named`, and the
patch is faulted dead — a legitimate patch killed by a module id. Either test automatic-label
prefixes (`src:`/`reify:`/`patch:`/`#`) before attempting the module split, or reserve those names
out of the module-id space.

### 5. NIT — :1304-1305 phrasing momentarily claims the installed copy keeps a durable path

"a `learn`ed clause sitting on a program `d.clause` rank does not become "program-side" and forfeit
the durable path" — read against the very next sentence (installed copies "must not promote under
the state banner", :1312-1315), the installed copy *does* forfeit direct durability; what survives
is the registered twin plus patch replay. The sentence is defensible only if "the durable path" is
read as the clause's identity path rather than the cell's — worth tightening to avoid a reader
concluding installed copies promote.

## Verified accurate (v1.5)

`insertIntoRank`'s already-present no-op (hpp:624-628 contract; cpp:1430-1432 `rankContains`
early-return) underlies the re-`learn` carve-out (:1324-1330) — correct as stated. `link`'s
two-sided eviction (:1347-1377) underlies the memo-key reasoning, the `d.patch-head` first-edge
justification (:1352-1361 — the patch's `+d.values` → descriptor occupancy is real, so a `+d.values`
first edge would evict it), and the synapse per-junction scalar (:1717-1720). The P16 gate cites
hold: handle pull-in :1759-1763, the `OpHandle`-ephemeral gate :1797-1809, `real.at(target)` for
ephemeral targets / verbatim for persistent :1947-1951, Int64 verbatim :1934-1936. `importValue`'s
unmarked-`OpHandle`→`None` demotion (:485-490) and relocation-marked local-twin naming (:494-501)
match :1533-1535's account. The label→head fallback to `code_named` for state-side registered heads
(:1440-1443) is coherent — the program-image walk cannot see them by construction. The "in either
direction" state-side claim matches `promote`'s `edge.pos`/`edge.neg` discovery (:1781-1783).
`exportSymbol`'s direct `sym +d.values → entryOp` (:119-126) keeps the head/op discriminator
load-bearing and correctly applied (:1434-1437, :1437).

VERDICT: **CONVERGED** — 0 blocking, 0 majors, 2 minors (single-numbering/program-image ingress;
agent-root exemption), 3 nits. Both r5 majors resolved verbatim with mechanically correct
replacements; the defect surface is down to specification polish.
