# R9 — ZigZag topology/promotion critique (v1.8)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.8 deltas in my lane: genesis (a)
positional assertion (:1727-1751), (d)→(e) install-set hand-off (:1765-1776), self-rehydrating
copies (:1779-1795), nested-literal label direction (:1826-1839), `home` path-reachability
(:1430-1444).

Verified against `arena_manifold.cpp` (`ensureDimension` links dim cells onto `home`'s `d.dims`
rank :91-103; `asInt64` kind-gated to `Int64` :1082-1087; `handleTarget` kind-gated to `OpHandle`
:1090-1097; `insertIntoRank` `link(anchor, dim, dir, cell)` + displacement :1423-1439; `promote`
BFS :1733-1788 — prune at **any** non-ephemeral cell :1755, `d.dims` skip :1766, both-direction
:1781-1783, relocation-handle enqueue :1759-1764, gate :1797-1809), `manifold.cpp` (`birthOp =
opIndex`/`byRef.emplace` :427-442; `home_` = first MakeCell :450-454 — "home first and d.dims
second, by fiat"), `vortex_vm.cpp` (`spawnCursor`: `link(rankTail(home, d.cursors), d.cursors,
…, cursor)` :148-161 — `home→cursor`; `linked(*cursor, d.stack, false)` :206 — `cursor→frame`;
opcode written as int64 *content* :136-146), `vortex_core.cpp` (`initGenesis` adopts
`*base->home()` :30-36; dims chained off `home` `+d.dims` :44-63), `mesh_value.cpp`
(`mergeValues` contentHash-sort + per-import `promote` :512-545; `importValue` unmarked
`OpHandle`→`None` :485-490, relocation-marked→`OpHandle` naming local twin :494-501),
`foreign_op_handle.cpp:98-117` (kind *and* marker).

## Verdicts on the v1.8 deltas (r8 adopted items)

- **`home` path-reachability (:1436-1443) — resolved.** The wording is now path-based ("no link
  path runs from the agent cell to `home`, the `+d.cursors` rank or any `d.stack` cell"), the
  edge-direction evidence is accurate (`home→cursor`, `cursor→frame`; membership edges
  `rankTail +d → member` — verified at `vortex_vm.cpp:157-160`,`:206`), and the conclusion is
  robust by a stronger mechanism than the sentence needs: `promote`'s BFS prunes at *any*
  non-ephemeral, non-`holdsOwn` cell (`arena_manifold.cpp:1755`), so even if a stray path
  reached `home` (`≤E`, persistent), expansion stops there and the frames behind it are
  excluded regardless — which is what makes the "holds either way" fallback (:1442-1443)
  honest. Residual nit below (N4).
- **(d)→(e) install-set hand-off (:1767-1774) — resolved, and it names a real traversal.** "Per
  patch, the minted install copy plus the `+d.refs` records `reify` regenerated on it" is a
  concrete hand-off: `reify` does regenerate the copy's `+d.refs` ("records remapped to the new
  ref cells, name-records re-linked", :1094), and the claimed necessity is precisely scoped —
  copies hang off `d.clause`/attachment anchors (program-side `≤E` cells the publish-time walk
  prunes *at*, `arena_manifold.cpp:1755`), so "no traversal over registered heads alone reaches
  them" is true (nothing links head→copy; `(e)` could alternatively enumerate the copies via the
  `+d.clause`-in-vocab `program()` walk, but the hand-off is the honest, simpler enumeration).
  `(e)` iterating install set + `d.code` rank is implementable. What the copies *carry* into the
  sweep is a separate problem — finding 2.
- **Nested-literal labels (:1826-1839) — resolved.** Outermost `src:` + `+o:c` arms descending
  inward is the right direction and matches §5.1's offset convention (`src:m:l:c+<off>:<off>`,
  :697; "nested literals chain offsets `+o:c` per level", :962). Uniqueness survives composition:
  clones take the expansion site's chain + kept `+o` arms + trailing `+n` (:1835-1839), distinct
  expansion sites give distinct bases, same-site clones are split by the ordinal, and the
  `#label`-in-multiply-expanded-body refusal (:2825) removes the one remaining collision class.
  The changelog's :2823 correction matches the body text.
- **Genesis (a) positional assertion (:1732-1741) — adopted in form, defective in substance.
  Finding 1.** On the interaction the round flagged: reify copies cannot perturb the check —
  they mint ephemeral cells at (d), *after* (a) runs, and `ephemeralBit|dense` refs can never be
  `≤E` persistent (`refOf`, `arena_manifold.hpp:468-470`); in the fold every cell is persistent,
  so "persistent" in clause 1 is vacuous at genesis. The check is unsound for its own reasons.
- **Self-rehydrating copies (:1785-1795) — outcome reachable, mechanism contradicted. Finding
  2.**

## New findings (v1.8 deltas only)

### 1. MAJOR — genesis (a)'s positional assertion has no consistent reading: the detection story needs the reach that also refuses every learned checkpoint

":1734-1736 every cell the `program()` structural walk reaches must hold a persistent ref `≤ E`,
and the walk's maximum must equal `E` — so an image twin minted above `E`, or any non-image cell
inside `1..E`, refuses genesis." The `program()` walk is defined at :1349-1361 as **outbound-
following** over a dim vocabulary that explicitly includes `+d.code`, `+d.refs`, `+d.recipe`,
`+d.name`, `+d.step`, `+d.values`, `d.clause`, `d.vars` — the `≤E` conjunct belongs to the
*sideness classification*, not to the walk's boundary.

Two readings, both broken:

- **Unfiltered reach (the only one under which the stated detections work):** the walk follows
  `agent +d.code → registered head` (rank membership `rankTail +d → member`,
  `arena_manifold.cpp:1423-1439`) and then the head's whole in-vocab record subgraph — all
  persistent cells `>E` on the design's own happy path. Any checkpoint of an agent that has
  `learn`ed even once contains exactly this shape, so clause 1 refuses it. (Patch cells and
  synapses escape only because `d.patch`/`d.synapse` happen not to be in the vocabulary — the
  registration surface is in it.)
- **Scoped reach (`≤E`-only transit, or "reached" = classified program-side):** clause 1 is
  vacuous and the claimed detections evaporate — an image twin minted `>E` is simply unreached,
  and "any non-image cell inside `1..E`" *is* `≤E`, so it passes both clauses (it is caught, if
  at all, only by the displacement of a reached cell above `E`). Detection then rests entirely
  on `walk-max == E`, which additionally false-refuses a *valid* lineage whenever the cell at
  position `E` is walk-invisible: dims live on `home`'s `d.dims` rank — the walk's named
  exclusion (`arena_manifold.cpp:91-103`, `vortex_core.cpp:44-63`) — `##` anchors are unlinked
  content cells (:1441), and nothing guarantees the band's last emitted op is link-reached.

So the sentence's own consequence clause requires the unfiltered reach that simultaneously
falsifies it. `spawn`-`learn`-`checkpoint`-`resume` cannot complete as literally written
(blocking-class), while under the charitable reading the check silently under-detects —
an implementer cannot tell which walk was meant. Proposed fix — state the check **per-edge**:
"every vocab edge the walk follows *from* a `≤E` cell lands on a persistent `≤E` cell — the one
designed exception being the agent cell's own `+d.code` registration rank, whose members are the
`learn`-registered state by contract — and the reached `≤E` set's maximum equals `E`"; and
either assert the emission invariant that position `E` is walk-reached, or better, promote the
in-paragraph ops-segment compare (:1740-1741 — "asserting the received ops `1..E` equal the
recorded image") from optional to primary, since it is the only stated form that is actually
closed. Knock-on: (b)'s registration scope — "every op cell in the `1..E` band the walk of (a)
just asserted" (:1754) — inherits the same ambiguity.

### 2. MAJOR — copy "pre-rehydration" contradicts §6.1's recipe model, and the correct mechanism is unstated

":1785-1790 a replayed install mints its slots with the registered head's *current* contents —
`reify`'s `New`/`Link`/`Value` replay cannot mint handle-kind cells, so a source `OpHandle` slot
contributes its handle *target* as plain Int64 and the copy's ref slots arrive pre-rehydrated"
(antecedent at :1769-1770, "the copy inherits the registered head's slot contents at mint").
§6.1 specifies the opposite replay: "v1 replays the *recorded emission template* — the recipe
the emitter wrote" (:1096-1098), and "copying the *current* graph (after in-arena mutation) is
the U-M7 walk". Under the recipe model a ref-box slot's value is a **recorded literal** — the
referent's emission-time dense id — which is stale post-resume (positions renumber; §4.5's own
table calls stale handles "aliased: names whatever cell landed at the old dense id"). Nor can
the `New`/`Link`/`Value` palette read the `OpHandle` slot live: `asInt64` is kind-gated to
`Int64` (`arena_manifold.cpp:1083-1087`); only `handleTarget` reads an `OpHandle`
(:1090-1097) — a kind-aware capture read the surgery palette does not price.

It is load-bearing, not cosmetic: if the copy's ref slot arrives carrying the stale Int64
literal, (e)'s ref-arm kind-guard ("skips cells already in its target kind", :1719-1720,
:1788-1789) *skips it* — the misbinding is frozen in — and (e) cannot repair it afterward:
the copy's regenerated ref-record carries `+d.ref → the copy's slot` but no handle; the
`handleTarget` it would need lives on the **head's** slot, a correspondence only (d)'s hand-off
could supply and currently does not name.

The sound path is already in the design but unstated: the recipe's capture records encode
endpoints as *cells* ("each record's `+d.values` points at the actual endpoint cells",
:1088-1089); at genesis the referent endpoint is the promoted fold cell — the same position
`handleTarget` carries — so `Value`-writing `int64(endpoint)` yields the rehydrated value with
no handle read at all. But that requires ref-box slot values to derive from the capture
endpoint, never the recorded literal — a constraint the text must state, because a naive
implementation of "literal contents" replay produces the permanent stale-alias bug. Proposed
fix: replace "inherits the registered head's slot contents" / "a source `OpHandle` slot
contributes its handle target" with the endpoint derivation — "ref-box slots are re-derived
from their regenerated ref-record's capture endpoint (`+d.values →` the referent, which folds
to the promoted cell — the identical value `*handleTarget` names)" — or extend the (d)→(e)
hand-off to carry the source-record correspondence so (e) writes
`Int64(*handleTarget-of-source-slot)` into copy slots explicitly.

### 3. MINOR — "fails closed" overstates the merged-lineage case

":1737-1739 A merged or re-seeded lineage … fails closed the same way" — under the only coherent
fixed reading of finding 1 (`≤E`-scoped transit), detection is probabilistic, not closed: a
scrambled fold is refused *unless* its `≤E`-reached component happens to include position `E`
and no in-vocab edge lands above the band. `mergeValues` scatters image twins in
contentHash-sorted import order (`mesh_value.cpp:519-543`), so refusal is overwhelmingly likely
but not a property of the check — the ops-segment compare already present at :1740-1741 is the
only form that is genuinely closed. Fix: either make the compare mandatory (preferred — it is
cheap and the adapter already holds the seeded segment), or soften "fails closed" to "is
refused whenever the scramble displaces a reached cell — the ops compare is the closed form".

### 4. NIT — the `home` claim is a construction statement that the prune, not the path-absence, makes robust

":1437-1438 no link path runs from the agent cell to `home`" is asserted of system-minted edges;
nothing in the design prevents a `Link` wiring a session cell into the cursors/stack chains if a
cursor or frame ref ever reaches program hands (the §4.5 handle-aliasing door already
acknowledged). The conclusion is safe anyway — `promote` prunes at any non-ephemeral cell
(`arena_manifold.cpp:1755`), so a reached `home` still cannot expose the frames behind it —
which means the load-bearing sentence is the fallback at :1442-1443, not the invariant. One
clause ("no *system-minted* edge creates such a path; the prune is what excludes the frames in
every case") would put the weight on the right leg.

## Verified accurate (v1.8)

The lineage paragraph's mechanics all re-verify: `opsAlreadySealed = 0` whole-spool seal
(`mesh_value.cpp:227-228`; `publication.cpp:611-617`,`:664-672`), `birthOp = opIndex` verbatim
fold (`manifold.cpp:434,442`), `home_` = first MakeCell (`manifold.cpp:450-454`), `initGenesis`
adopting base home (`vortex_core.cpp:30-36`), `importValue`'s unmarked-`OpHandle`→`None` and
marked→local-twin arms (`mesh_value.cpp:485-501`), `mergeValues`'s sorted promote-into-fresh-store
(:512-545). The (d) replay ordering rationale (:1791-1795 — name-record rebinds must see
post-installation symbols) verifies: a patch-replaced `graft_method` re-point replayed at (d)
writes the symbol's `+d.values` before (e)'s sweep resolves names through it, which is exactly
the misbind class the reorder removes. `(e)`'s contrast against `importValue` (:1779-1784) is
accurate — `resume` receives `OpHandle`-kind cells verbatim (`real.at(target)` promoted refs)
where `importValue` re-mints local twins. Copy-inheritance does *not* threaten (a)'s band:
installed copies are ephemeral, minted after the assertion, and never reach the `1..E`
positions. The `patch:`-arm/descriptor, `plan_named` `run`-operand removal, and
`code_named`-fallthrough edits adjacent to my lane read consistently with the mechanics.

VERDICT: NOT CONVERGED
