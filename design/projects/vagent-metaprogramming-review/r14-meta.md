# Round 14 critique — `design/vagent-metaprogramming.md` v1.13 — Metaprogramming Expert lane

Scope: verify R13-A (`d.captured` dropped, endpoints content-carried — :1139-1169, :1040-1059,
:2195-2234) and whether genesis (e)'s content-read of the paired slot rehydrates all three
referent classes with no edge into shared referents; verify my two r13 minors (comparand
pronoun :1597-1607/:1984-1988; equal-text `patch:<n>` labels :1741-1744/:2181-2182); judge
whether the recipe/ref-capture model is now closed; hunt new defects in the v1.13 deltas only
(full diff `git diff 5dd90207..f252c2b8` reviewed; code citations re-verified against
`apps/common/xanadu/`).

## 1. R13-A verification — the fix is sound and the model is closed

**The mechanism.** The ref-capture record now carries only the correlation edge (`+d.refFor →
the same slot cell`, :1139-1147); the referent rides *content*: internal via the slot's `int64`
+ the old→new map (map membership is the discriminator), external/state-side via the slot's
verbatim `int64`/`*handleTarget`, program-side dim via `+d.name →` the name string on the
recipe record, re-resolved through `ensureDimension`'s name arm. Checked against the code:

- **No edge into the referent exists anywhere.** Shared referents are now N slots carrying the
  same `int64` — content, not links — so the single-occupancy `-d` backlink that killed
  `d.captured` is never touched. The only survivor is the *optional* internal-arm
  `+d.values`-may-name edge, occupancy-guarded ("only where the endpoint's `-d.values` is
  unclaimed"; a shared endpoint takes none) and never load-bearing — the slot's content carries
  the endpoint "regardless" (:1156-1157). Cross-head sharing (the `reify`-copy severance that
  poisoned every state-side capture in v1.12) is gone by construction.
- **Durable paths, all classes.** (i) *Internal* — the dominant class: `bindInput`/`reference()`
  write `int64(operand)` for control-flow targets, spill slots and shared scalars
  (`compiler.cpp:516-517,525,531-532,769-772,781,796`); the marked handle enqueues the target
  into promote's frontier (`arena_manifold.cpp:1759-1764` — `isCellRelocationHandle` →
  `seen`/`order`/`frontier`), so `*handleTarget` post-resume *is* the referent's fold position;
  at replay the map rebinds to the copy twin. (ii) *External/state-side* — `cells"…"`
  anchor-resolved cells (`reference(*subject)`, `compiler.cpp:832`): verbatim position;
  `promote` writes `real.at(target)` for ephemeral targets and the verbatim persistent id for
  base targets (`arena_manifold.cpp:1947-1951`, gate `:1797-1809`), genesis (e) writes
  `Int64(*handleTarget)` back. (iii) *Program-side dim* — `+d.name →` dim name →
  `ensureDimension`'s name arm (`dimensionNamed` → `arenaDims_` → mint,
  `arena_manifold.cpp:49-105`); durable by name, the only arm immune to position drift.
  (iv) `cap$N` captures ride §5.3's clone/`masterOf` links (:819-825), which fold verbatim —
  a separate, already-durable path needing no record. (v) Program-side non-dim int64 referents
  and frame-bound captures are refused by `closed`/binding class — correctly pathless.
- **(e)'s sweep is right.** Registered heads: `Int64(*handleTarget)` for internal+external
  (:2201-2204). Replayed copies: internal → `int64(map[srcCell])` to the fresh twin; external →
  the donor slot's promoted target verbatim ("an equality that holds for the external arm
  only", :2222-2224); the copies arrive Int64-kind, so the ref arm's kind-guard no-ops them and
  only their name-records rebind — the reason (d) hands its install set over (:2194-2199,
  :2228-2230). Ordering after (d) keeps post-replay installs visible (:2230-2234).
- **Occupancy reasoning still holds.** `+d.refFor` is the sole record→slot edge on a free
  backlink; the `+d.values`-may edge is the only new occupancy question and it is correctly
  conditioned (descriptor `+d.values → result/produced` already claims those cells — exactly
  the case the guard refuses). The r13 composition proof is unaffected: verb replay, alias
  dedupe, and the verbatim walks are untouched by this fix.

**Answer to the assigned question: yes — genesis (e)'s content-read rehydrates all three
classes without any edge into shared referents, and the recipe/ref-capture model is closed.**
Capture-as-content + `d.refFor` correlation + old→new map + `ensureDimension` name-arm covers
every referent class that `closed` admits; every class it does not cover is one `closed`
refuses.

## 2. My r13 minors and the named fixes — landed

- **Comparand pronoun — FIXED.** :1600-1604 now pins roles: the seeded lineage store "supplies
  the reference band, never the publish destination"; :1986-1988 reads "verifies the rebuilt
  store's decoded `1..E_ops` band against the lineage's decoded reference band". Reference =
  lineage, checked = rebuilt, consistent with (a)'s comparand (:2080-2083).
- **"share the `patch:<n>` `+d.name`" — FIXED.** :1741-1744 "carry equal-text `patch:<n>`
  labels on their own `+d.name` label cells"; :2181-2182 "two equal-text `patch:<n>` label
  cells ... one on the record and one on the installed copy"; the contended slot correctly
  named "each label cell's `-d.name` backlink" (:1767).
- **Other adopted minors verified in place**: `globalKeyOf` arm set (:2086-2094 — verified:
  `publication.cpp:853-869` has breakMarker/local/external arms; `localScroll`+
  `breakMarkerScroll` remap at `:1049-1050`; `sealableOps` skip `:985-987`;
  `breakMarkerKey = "break:"` `publication.hpp:447`; `Store::scroll` disengages on
  `localScroll` or `id > externals.size()` `store.cpp:1461-1469`; `localScroll == 0`
  `spool.hpp:19`); alias "no session resolver consults" (:1196-1197); alias conditional on
  dedupe hit (:1225-1228); join multiplicity latest-wins (:1202-1205); `label_of`/genesis
  handle split (`unlearn` never matches the label, :1763-1764, :1776); `textOf` claim
  (:1771-1773 — `vortex_stdlib.cpp:1953` matches symbol content, verified); divergent-victim
  replay dedupe (:1233-1237, :2188-2190 — worked through: a session append replays as displace
  when an intervening same-rank displacer's patch replay-drops on a dead target — e.g., a dead
  reify source — resurrecting its folded victim under the later patch's alias; deterministic
  and canonical-rank correct); `learn_plan("Patrol","demo:Guard.cautious")` premise (§9.2
  declares `plan cautious for Patrol` on `demo`'s `Patrol` goal, :2597-2604); `documentId`-arm
  (v) consistent with §10.1 (:1551-1559, :2762-2768; `historyFromSeal`'s internal mint verified
  `publication.cpp:1097`, `Store(permascroll, documentId)` ctor `store.hpp:132-133`,
  `restorePublication` precedent `:1127-1128`); `set_weight` mints-on-unkeyed + `else` seed
  (:2445-2452, :2651); `d.refFor` restored to §10.1 (:2788) and §14 R12 (:3028); `d.captured`
  struck from all minted-dim lists (:95, :533, :1018, :2788) and present only in changelog
  history.

## 3. New findings (v1.13 deltas only)

### 1. MINOR — §6.6 (e) :2200-2206 + :2213-2214 — the program-side writeback value is unstated, and the uniform rule gives the wrong one in the divergence case

"per ref-record `Int64(*handleTarget)`" is spelled for internal and external; for
program-side the text says only that the record's `+d.name` is "re-resolved through
`ensureDimension`'s name arm" — never that the slot takes `Int64(the resolved dim's
position)`. The general rule that follows — "rehydration `writeValue(slot, Int64)`s each
marked cell back with **the promoted target**" (:2213-2214) — invites `*handleTarget` for the
dim slot too. The two candidates diverge: a session-minted dim folds *without* its `d.dims`
rank membership — promote's frontier walk skips `d.dims` edges outright
(`arena_manifold.cpp:1764-1766`: `if (from.textOf(edge.dim) == "d.dims") continue;`), so the
dim reaches the fold only via the slot's own `handleTarget` enqueue — leaving a durable but
name-unfindable orphan; post-resume `dimensionNamed` then misses it and `ensureDimension`
mints the canonical twin at a different position. Since a `DimRef` is a cell identity, not a
name, a slot written `*handleTarget` binds the orphan while every name-resolved consumer sees
the twin — a silent dim-split for `Linked`/`New` traffic. **Fix:** state the write per arm —
internal/external `Int64(*handleTarget)`; program-side `Int64(the name-arm-resolved dim's
position)`, optionally asserting equality when the promoted dim is name-findable.

### 2. MINOR — §6.6 :1839-1848 (echoed :137) — `closed`'s admitted-reference enumeration is stale against the content-carried classes

Arm (a) admits "a link to a *state-side* cell — a capture or `cells"…"` path resolved at
emission"; arm (b) admits "a *name-record* on `+d.refs`". The new §6.1 enumeration makes two
admitted classes that fit neither literally: external/state-side referents ride slot `int64`,
not links (`reference(*subject)` mints `int64`, `compiler.cpp:832` — a `cells"…"` anchor is
not a link, though the ref-record is exactly what makes the int enumerable per §5.3's "an int
needs a record to be found"); and a program-side dim's `+d.name` lives on the `+d.recipe`
ref-capture record, not on `+d.refs`. The *operative* check (:1868-1870 — "checking each
endpoint is state-side or a name-record") covers both — a session dim promotes `>E_ops` and a
`cells`-anchor endpoint is state-side — so the mechanism is recoverable, but the definitional
(a)/(b) dichotomy, applied literally, refuses external-int64 and persistent-dim referents the
model now explicitly admits. **Fix:** restate the admitted shapes — (a) state-side endpoints
however carried (capture link or slot int64), (b) `+d.refs` name-records, (c) program-side
referents named on the paired ref-capture record's `+d.name` — and have the frontier
enumeration (:1863-1864) note ref-capture endpoints are read through slot content.

### Nits

- :2114-2121 — "the walk reaches them through capture edges — a fragment's slot link to an
  agent-`d.vars` binding cell" should name the surviving mechanism: §5.3's clone/`masterOf`
  capture-slots (and emission-resolved links). Post-R13-A "capture edge" is ambiguous — the
  ref-capture record no longer carries one, and int64-carried referents are never
  link-reached; the `closed` frontier reaches those through content, not edges.
- :2195-2196 — "per patch, the minted install copy" still reads clause-installs-only;
  `learn_where`/`learn_body` re-points mint head copies too, and their regenerated
  name-records need (e)'s rebind exactly as install copies' do (carried from r13): "every copy
  a replayed verb mints, installed or re-pointed".
- :1227-1228 — "the field names a superseded clause exactly when the install-rank dedupe hit"
  is tautological: hit and append record the identical value (alias = user name = victim label
  on a hit), so displaced-vs-appended is derivable from rank history/replay, not auditable in
  the record. Say so, or carry a `displaced` flag.
- :1155-1157 — the `+d.values`-may-name arm has no named consumer (replay uses slot content +
  map; `closed` uses slot content); "may" is nondeterministic across emitters. Pin "does name
  ... where free" for recipe self-description, or drop the arm.
- :1747-1749 — "the alias present only where the verb takes a `name` argument" is literally
  false — `learn_method` takes `name` — before the parenthesis corrects it; "where the verb
  takes a *user-alias* name" is the accurate lead.
- Carried: the durable counter's cell shape on the agent cell's `d.vars` is still unstated
  (:2300-2303) — a named member would join the first-match `d.vars` walk a user member can
  collide with; say "adapter-minted anonymous scalar record".
- Citations: `scroll.cpp:13-22` for `scrollKey` — signature at :12 (carried);
  `publication.cpp:1097` for the `historyFromSeal` mint now verified exact.

## 4. Staging-discipline summary

R13-A is the right fix and it is landed correctly: by removing the referent edge entirely —
not re-dimming it — the design eliminates the collision class rather than relocating it, and
every referent class retains a durable path (map for internal, position for external, name for
dims, links for `cap$N`, name-records for call/run). The five-lane convergent ruling has
produced the cleanest version of the recipe model yet: `d.refFor` is the only correlation,
content the only carrier. The residuals are wording-level: (e) must pin the program-side
writeback to the name-arm result (the one place `*handleTarget` and the resolved referent can
diverge), and `closed`'s definitional enumeration needs the content-carried classes written in
— both one-clause fixes.

VERDICT: CONVERGED — 0 blocking, 0 major, 2 minors (program-side (e) writeback unspecified;
`closed` (a)/(b) enumeration stale vs content-carried external/dim referents), 7 nits.
