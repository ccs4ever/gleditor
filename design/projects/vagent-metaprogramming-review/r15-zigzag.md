# R15 — ZigZag topology/promotion critique (v1.14)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.14 deltas in my lane: the agent-legs
restatement (:2134-2143), the "capture edge/link" vocabulary sweep (:137, :1853-1854, :1880-1882,
:2139-2140, :3603-3605), the recipe-tuple `+d.values` conditional (:1137-1142), the (d) hand-off
(:2214-2222, :2238-2255), the `closed` program-side carve-out (:1859-1864), the `*handleTarget`
(d)-time read (:2241-2245), the displaced-vs-appended bound (:1234-1237), the grafted-twin replay
caveat (:1244-1247), the "no session resolver" mirroring (:1240), and the `documentId_` narrowing
(:634-637, :1561-1564, :2859-2863).

Verified against `arena_manifold.cpp:1934-1936` (`promote` mints `Int64` verbatim — no position
translation for content), `:1947-1951` (`OpHandle` → `real.at(target)`), `:2020-2052` (edges
retargeted through `real`), `:1759-1764` (relocation-handle frontier), `:1347-1377` (link evicts
both ends' occupied same-dim neighbours — M14), `vortex_core.cpp:344-346` (`masterOf` =
`cloneMaster(cell, dims_.clone)`), `:356-365` (`wingOf` operand lambda: `asInt64` → ref, else
`masterOf(slot)` — the `cap$N` decode §5.3 :822-826 cites), `store.cpp:130-135` (two-arg ctor
writes `documentId_` :135), `:4450` (`documentId_ = tables.documentId` on directory load),
`publication.cpp:1097` (`historyFromSeal` mints `Store()`), `compiler.cpp:769-772` (`dimConstant`).

## Verdicts on the r14 dispositions

### Agent-legs restatement (:2134-2143): **resolved — accurate**

"named `>E_ops` legs hanging off the *agent cell* are reached only where a runtime-grown edge leads
to them — `program()` and `agent_cell()` are distinct adapter-bound roots never `+d`-reached from
each other … an agent-`d.vars` binding cell hangs on the agent cell's own `±d.vars` rank and enters
this walk only across a genuine cross-root edge — a §5.3 `cap$N` capture slot resolving to its
binding cell through `masterOf`/`d.clone` is the surviving case, a ref-capture record contributing
none since its referent rides slot *content* the link walk cannot follow — and every such leg
classifies `>E_ops`/state-side regardless."

All three clauses check out. The two-root split is the adapter's own model (`agent_cell()` is
bound, not `+d`-linked, so the program-root walk cannot reach it). The `cap$N` mechanism is genuine
edge structure — §5.3 :822-824 emits a capture-slot cell on the operand's wing (`+d.grab`/`+d.step`)
cloned from the binding cell, decoded by `wingOf`'s operand lambda (`asInt64` fails →
`masterOf(slot)`, `vortex_core.cpp:356-365`; `masterOf` reads `dims_.clone`, `:344-346`) — so it is
a real link the walk can follow, unlike the content-carried ref-capture referent, which the doc's
own bound calls "invisible to any link walk" (:2180-2181). The R14-A defect — claiming reachability
"through capture edges" of the class R13-A abolished — is gone; the surviving claim names the one
edge class that actually exists. The conclusion (legs classify `>E_ops`/state-side, no agent-rooted
second walk owed) is unchanged and remains right. One residual nit: Finding 2 below.

### Vocabulary sweep: **resolved — clean**

`capture edge`/`capture link`/`cell link`/`slot link`/`correlation edge`/`d.captured` survive only
where genuine: `cap$N` capture links (:137, :1853, :1881, :1690, :3281, :3605, :3624) — a real
wing-dim edge class per §5.3 — and change-history recounts (:3536-3539, :3574-3587). The three
sites my r14 Finding 3 named are fixed: glossary :137 now reads "reached by a `cap$N` capture link …
or carried as a ref-box slot's `int64` content"; §6.6's frontier (:1880-1882) reads "`cap$N`
capture-slot links, the `+d.recipe` ref-capture records whose endpoints are read off their paired
slots' content"; the `closed` arms (:1853-1854) distinguish edge-carried `cap$N` from
slot-content `cells"…"` referents. `d.captured` appears only in changelog recounts. The 1.13
entry's own "capture edges" phrasing is corrected (:3603-3605).

### (d) hand-off (:2214-2222, :2238-2255): **resolved — coherent**

:2214-2217 now reads "the copy's ref slots derive their endpoints from the *paired donor slot's
promoted content* at replay … the record supplying the pairing and the referent class (`+d.refFor`,
the conditional `+d.values`, `+d.name`), never the endpoint value itself" — the r14 inversion is
fixed, and it agrees with the (e)-coverage restatement at :2238-2245: the referent "is read off the
paired slot's *content* … `int64` ref position in-session and its `*handleTarget` at (d) — the
donor's marked ref cells still `OpHandle`-kind, (e) rehydrating last." Accurate: donor slots arrive
`OpHandle`-kind carrying `real.at(target)` (`arena_manifold.cpp:1947-1951`), `handleTarget` reads
`valueBits` (`manifold.cpp:1115-1122`), and (e)'s last-pass ordering is what keeps them readable.
The `*handleTarget`-at-(d)/`int64`-in-session split is stated. The hand-off rationale — install
copies hang off pruned attachment anchors, so (e) must iterate (d)'s install set — is coherent.

### `closed` program-side carve-out (:1859-1864): **resolved**

Arm (c) now states the dim-referent case: `+d.name` on the paired *ref-capture* record, "the
endpoint being the named dim rather than a cell and counting as name-resolved, the slot's own
`+d.refs` record staying a REF-record" — consistent with :1046-1051 (the `+d.name`-on-`+d.refs`
discriminator) and :1166-1170 (`ensureDimension`'s name arm). The :1888-1889 re-verification clause
names both record arms.

### Other adopted minors: **all verified applied**

Displaced-vs-appended bound scoped to *declared* victims (:1234-1237 — the folded-back `+d.name`
gives the join something to find; session-grafted victims read append). Grafted-twin caveat kept
(:1244-1247 — session append replaying as displace on a replay-dropped intervening patch).
"No session resolver consults the alias" mirrored (:1240). `documentId_` narrowed to
"ctor-or-load state" in both places (:634-637, :1561-1564) — `store.cpp:135` two-arg write and
`store.cpp:4450` load write both cited and accurate. Register item (v) cites the `Store()`
signature at `store.cpp:123` (:2861) — consistent with the settled cite.

## New findings (v1.14 deltas only)

### 1. MAJOR — the recipe-tuple `+d.values` conditional splits the endpoint keyspace across `promote`: the `int64` fallback freezes at the emission position while every other naming folds to the promoted one

`:1137-1142`: "each endpoint field pointing `+d.values` at the actual endpoint cell under the same
one-record-one-edge rule the ref-capture arm below states — a claimed `-d.values` taking no edge,
the endpoint then riding the field's `int64` content for the old→new map to translate either way
(an op cell is `to` of one tuple and `from` of the next, so shared endpoints are routine) — never
a census integer (checkpoint renumbers dense ids, §4.5)".

The conditional is mechanically *necessary* — M14: a `+d.values` edge into an endpoint whose
`-d.values` is claimed evicts the incumbent (`arena_manifold.cpp:1347-1377`), and shared endpoints
are routine as stated. The problem is what `promote` does to each carrier — the two arms land in
different position spaces at (d):

- **Edge-carried** (`-d.values` free): the `+d.values` edge folds through `real`
  (`arena_manifold.cpp:2030-2042`) and post-fold names the endpoint's *promoted* position — the
  same space the replay map is forced into, because the marked ref-box slots read `*handleTarget`
  = promoted position (:2241-2243, `arena_manifold.cpp:1947-1951`) and every edge naming in the
  same recipe folds the same way.
- **`int64`-carried** (`-d.values` claimed): `promote` writes `Int64` *verbatim*
  (`arena_manifold.cpp:1934-1936`) — the recipe field is not a marked ref cell (marking keys on
  `+d.refs` records, :1063-1065), so post-fold it still names the emission-time *ephemeral*
  position: a dead ref, ephemeralBit set, naming nothing in the received fold.

So at genesis (d), a shared endpoint's first naming (edge) reads the promoted position and hits the
map while its second+ namings (int64) read the dead emission position and miss — a verbatim stale
ephemeral ref fed to `Link` (dead-ref failure, or worse, a recycled ephemeral index aliasing a live
session cell — the stale-alias class the endpoint derivation exists to preclude). The symmetric
encoding is no better: if the map were keyed by emission positions instead, every *edge*-carried
endpoint would miss and the copy would `Link` into the folded *source* — aliasing the original
across the copy boundary. In-session the two keyspaces coincide (the emission position *is* the
live position), which is why the defect is invisible at `learn`-time and bites only on the (d)
path — the one this section exists to serve. And the sentence's own justification undercuts the
fallback: "never a census integer (checkpoint renumbers dense ids)" — the substituted cell ref is
renumbered by the same fold; it survives only as a *never-dereferenced map key*, which the spec
doesn't say.

The root cause is borrowing the ref-capture arm's occupancy rule without its survival mechanism or
its carrier discipline: the ref-capture `int64` rides a *marked* slot whose content promote keeps in
current position space (`*handleTarget`), and there "the slot's content carrying it *regardless*"
(:1162) — content always load-bearing, edge decorative. The tuple sentence inverts that: edge
primary, content the claimed-only fallback — and there is no marker on the field.

**Fix** — pick one uniform keyspace; the cheap one mirrors the cited arm verbatim: "each endpoint
field carries the endpoint's *emission position* as `int64` content — the uniform map key, never
dereferenced — and may additionally point `+d.values` at the endpoint for self-description where
its `-d.values` is free, one record one edge, the replay never resolving the edge for translation;
the cell-records name their sources by the same `int64` key so every endpoint naming lands in one
keyspace." (Heavier alternative: extend `d.mesh.relocation` marking to recipe endpoint fields so
their content re-kinds `OpHandle` and reads `*handleTarget` = promoted position — uniform folded
keyspace matching the edge reads, at the cost of growing the mark ceremony beyond `+d.refs`.)

### 2. MINOR — "a genuine cross-root edge … through `masterOf`/`d.clone`" leaves the traversal direction unstated: the edge leads *out of* the binding cell, so the leg is reached by clone-decode, not a posward edge

`:2138-2139` says agent-`d.vars` legs are "reached only where a runtime-grown edge leads to them —
a §5.3 `cap$N` capture slot resolving to its binding cell through `masterOf`/`d.clone` is the
surviving case." The edge direction runs the other way: `Clone` appends to the source's `d.clone`
rank (:1184-1185), so the binding cell `+d.clone` → capture slot — the hop from slot to binding
cell is the `-d.clone` backlink, which `masterOf` reads (`vortex_core.cpp:344-346`). If the
sideness walk is posward-only it reaches the *slot* (the clone) and never the binding cell; the
stated case needs the walk to apply `masterOf`-decode on reached clones, which is semantic deref,
not edge-following. Harmless to the conclusion — the slot itself is `>E_ops`/emission and
classifies identically — but the sentence currently papers over whether the walk resolves
`masterOf` or stops at the slot. Fix: "…a §5.3 `cap$N` capture slot on the wing — the walk reaches
the slot posward and its binding cell by the `masterOf`/`d.clone` decode §5.3 states — is the
surviving case; legs classify `>E_ops`/state-side either way, so the decode's reach changes no
classification."

## Verified accurate (v1.14)

`promote`'s three translation arms (`arena_manifold.cpp:1934-1936` Int64 verbatim, `:1947-1951`
`OpHandle`/`real.at`, `:2020-2052` edge retarget) — the basis of Finding 1. M14 both-ends eviction
(`:1347-1377`). `cap$N`/`masterOf`/`d.clone` decode (`vortex_core.cpp:344-346,356-365`). The
marking rule's `+d.refs`-record antecedent (:1063-1065) — recipe fields outside it. `documentId_`
write sites (`store.cpp:135,4450`). `historyFromSeal`/`Store()` (:2861 cites, `publication.cpp:1097`,
`store.cpp:123-125`). Two-root adapter model; `±d.vars` rank membership for binding cells. The
(d)/(e) ordering invariant — donor slots `OpHandle`-kind until (e)'s `Int64` writeback. `d.captured`
absent from all live text; `d.refFor` in all enumerations.

VERDICT: NOT CONVERGED
