# R13 — ZigZag topology/promotion critique (v1.12)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.12 deltas in my lane: the
`d.refFor`/`d.captured` ref-capture re-key (schema :1050-1057, mechanism :1136-1158, genesis (e)
:2163-2167, changelog :3460-3468), the durable dedupe via verb-replay (:1214-1223, :2150-2155),
the two equal-text `patch:<n>` label cells + re-mint transport (:1741-1747, :2143-2149), the
(d) serial scoping (:2138-2140), the per-verb `+d.name`/serial split (:1738-1750), the
`unlearn` `-d.patch` refusal (:1112, :1794-1797), the §10.2 segment-apply registration
(:2785-2793, :2805-2810), the (a) comparand harmonization (:1585-1587, :2046-2066), the
`d.weight` de-ranking (:1494-1496), and the `≤E_ops` agent-cell scoping (:1499-1504, :2082-2090).

Verified against `vortex_core.cpp:366-368` (`wingOf` reads `linked(opcode, dims_.grab, side)`
first, returns `{}` when absent), `vortex_core.cpp:384-390` (`bindInput` first slot writes
`opcode +d.grab → slot`), `arena_manifold.cpp:1347-1374` (`link` clears the displaced occupant's
side — the M14 eviction the doc invokes), `store.cpp:1460-1467` (`Store::scroll` optional,
disengages on `localScroll == id || id > externals.size()`), `spool.hpp:19` (`localScroll = 0`),
`scroll.cpp:13-21` (`scrollKey` named/file arms), `store.cpp:123-125` (`userPermascroll_` mint
:123-124, `chronofilade_` :125 — the doc's corrected cite is accurate), `publication.cpp:1097`
(`historyFromSeal` `Store` mint). The remaining cites carry over verified from r12.

## Verdicts on the r12 items

### R12-A — `d.refFor`/`d.captured` replacing `+d.grab`/`+d.values`: **`d.refFor` sound, `d.captured` still occupancy-broken on shared referents — residual MAJOR (Finding 1)**

`+d.refFor` is the right shape. It is a dedicated dim; the slot's `-d.refFor` backlink is free
by construction (nothing else writes it); and the doc's premise — "a slot pairs with at most
one ref-record" (:1144-1145) — holds: each marked ref cell takes exactly one `+d.refs` record
whose kind (name vs ref) is decided once at emission, and ref-capture records are "one per
`+d.refs` ref-record" (:1050-1052), so `-d.refFor` can never contend. It also does not collide
with the wing dims it replaces: the `+d.grab` eviction pathology (`bindInput` occupies a first
input slot's `-d.grab`, `vortex_core.cpp:384-390`; `wingOf` decapitates on the absent head,
`:366-368`) is named accurately at :1139-1144. Per-record single occupancy is structurally
sound on this arm, at emission and at replay (the copy's slot is fresh).

`+d.captured` is not — the referent endpoint is a shared cell, and the fix only analyzes the
intra-head, exactly-two-sharers case. Finding 1.

### R12-B — durable dedupe via verb-replay: **resolved**

The adopted mechanism — the record's `alias` field doubles as the recorded superseded-target
name; genesis (d) re-runs each patch's *verb*, re-matching the alias verbatim against the
re-attached rank and re-unlinking the hit (:1214-1221, :2150-2155) — is consistent, and the
divergence I probed does not materialize. Rank order is load-bearing and it holds: declared
clauses sit at image positions and session grafts only append at `rankTail`
(`arena_manifold.cpp:1423-1439` convention, :1730-1734), so a verbatim first-match at emission
hits the declared clause whenever one carries the name — grafted hits occur only when *no*
declared clause matches, and those need no re-enactment because grafted cells never fold back
(replay's no-hit correctly appends). The `learn_method` stack replays correctly too: learned
symbols keep `owner.name` content (:1746), so patch:2's unlink-first legitimately removes
patch:1's replayed symbol. The in-session dead-clause asymmetry is stated, not hidden
(:1221-1223: unlearn does not re-link the displaced clause; it returns at next genesis only if
the displacing patch was unlearned). Scope pinned to the install goal's own `d.clause` rank
(:1201-1203); the self-contradiction is gone ("any verbatim hit: declared or grafted, qualified
or unqualified" :1204-1205); the surviving ambiguity is correctly restated as graft-on-graft /
graft-shadowing-declared with *both* `plan_named` first-hit and `unlearn` latest-name named
(:1228-1231).

### R12-C — kept-twin segment-apply surface: **resolved**

Registered twice: enumerated as adapter-side surface (iii) beside the core register
(:2787-2791) with the correct conditional ("owed only if the kept-in-step adapter twin of
§6.6's seam is kept — the `base()->store()` verify arm stands alone without it"), and spelled
out below the register (:2805-2810: `adoptOpRecords` takes decoded records, `historyFromSeal`
mints its own `Store` at `publication.cpp:1097`). §6.6's forward reference now resolves.

### R12-D — comparand harmonization: **resolved**

Canonical statement lives in (a) (:2052-2066): order-vs-order post-remap fields, op kind plus
non-scroll fields verbatim, scroll *identity* by `scrollKey` under `Store::scroll`'s
`localScroll` disengage (`store.cpp:1460-1467` — optional-returning, `localScroll == id` →
nullopt; `spool.hpp:19`; `scroll.cpp:13-21`). §6.6's verify arm cites it verbatim rather than
restating a different comparand (:1585-1587). One comparand, one site.

### Minors — **all resolved**

Serial pinned to the *agent cell's* `d.vars` counter everywhere (:136, :390, :1111, :1178-1179,
:1718-1719, :1739, :1810, :2617). Re-mint transport correct (:2143-2147): a fresh label cell
written with the record's text, never a re-link — the occupancy reasoning is exactly right,
since re-linking `copy +d.name → record's label` would evict the record's `-d.name` backlink
and drop `patch:<n>` at second genesis. Two equal-text label cells stated (:1741-1743,
:2146-2147), no `plan_named` collision (its space is clause-rank cells; the record's label is
on `d.patch`). `(d)`'s no-serial claim scoped to `patch:<n>` with the `reify:<src>:<serial>`
mint acknowledged (:2138-2140) — consistent with §6.1's unconditional label mint (:1164-1165)
and monotone either way. Per-verb `+d.name`/serial split stated (:1738-1750): every `learn_*`
consumes `<n>` and every record carries the label; clause-installing verbs mint the second
equal-text label on the installed clause while `learn_where`/`learn_body`/`learn_method` keep
it record-only. Alias→clause join spelled (:1190-1194: patch-rank `alias` key-record →
`ordinal` field → `plan_named("patch:<n>")`). `unlearn` `-d.patch` refusal (:1112, :1794-1797):
the backlink cleanly discriminates — clause args carry `-d.clause`, symbol args `-d.vars`,
only `d.patch` rank members carry `-d.patch` — and the refusal's motivation (the record's
equal-text `patch:<n>` would accidentally match the clause-arm patch) is now *required* given
the two-label design. `d.weight` de-ranked to the §8.1 scalar edge dim (:1494-1496).
`agent_cell() ≤ E_ops` exempt case scoped (:1502-1504). Citation nits applied:
`real.emplace` `arena_manifold.cpp:1867-1868` (:1583), `store.cpp:3840` (:2105),
`store.cpp:123-125` verified accurate, the ordinal now read "off the patch *record's* `ordinal`
field" (:1191-1192), "publish `Store`"→"lineage `Store`" (:1512), "the hydration"→"the
span-source pair" (:1517-1519).

## New findings (v1.12 deltas only)

### 1. MAJOR — `+d.captured`'s shared-referent arm breaks M14 one level up: "the earlier record"+"one hop" coheres only for exactly two same-head sharers, and cross-head sharing is unhandled

`:1149-1153`: "a shared referent is named once, later records carrying `+d.captured → the
earlier ref-capture record` (a recipe-chain member, discriminated by its chain membership
since no referent sits on this head's recipe chain) whose `+d.captured` the replay follows one
hop". Three defects in one mechanism, all the same M14 displacement the fix exists to remove
(`link` clears the displaced occupant, `arena_manifold.cpp:1347-1374`):

- **≥3 same-head sharers.** If later records all point at *the first* record (the only shape
  where "one hop" is literally true — record→first-record→referent), the third record's
  `+d.captured → A` evicts the second's (`A`'s `-d.captured` is single-occupancy) — B silently
  loses its endpoint at *emission*. If "the earlier record" means the *immediately-preceding*
  record for that referent (chain `R←A←B←C`), occupancy is sound and termination guaranteed
  (strictly earlier records, finite), but the read is a multi-hop walk — "one hop" is then
  factually wrong in all three restatements (:1151-1152, :2166, :3466), and (e)'s
  `+d.captured → the endpoint (one hop on a shared referent)` (:2166) dereferences once and
  lands on another *record*, writing `int64(record)` as the referent.
- **Cross-head shared referents are unhandled — and they are the standard path, not an edge
  case.** Every `learn_*` mints *two* `reify` copies (:1111), and every `reify` output carries
  its own `+d.recipe` with ref-capture records (:1104, :1163-1165): for a state-side referent,
  "external captures are preserved verbatim" (:1161-1162), so the sibling copies' records both
  write `+d.captured → R` and whichever reifies second evicts the first's edge — benign only
  if the *registered* copy reifies last (:1111's ordering suggests it; :1715-1717's
  parenthetical "(registered head, installed copy)" suggests the opposite, and the order is
  nowhere stated as load-bearing). Worse, across separate `learn_*`s: two registered heads —
  or re-learning the same literal, which "mints a second registered copy" (:1692-1694) —
  capturing one `d.vars` binding cell (the flagship case: two learned clauses reading one
  belief) makes the later recipe's `+d.captured → R` evict the earlier registered head's live
  edge; the (e) sweep then finds a record with no endpoint, and each `reify` of either head
  re-severs the other's edge in-session. The parenthetical's discriminator — membership on
  "this head's recipe chain" (:1151-1152) — confirms the analysis stopped at the head
  boundary: under a global-chain reading, a `+d.captured` target on *another* head's recipe
  chain fails that membership test and is misread as the endpoint.
- **Copy-recipe remap coverage unstated.** The copy's recipe is "endpoints remapped"
  (:1164-1165), but a `+d.captured → earlier record` endpoint is a *recipe-record cell* — the
  link-tuple endpoints the old→new map covers are graph cells (:1158-1160). If the map lacks
  record cells, the copy's chained edge points at the *source's* record, and rehydration
  follows it to the source's endpoint — writing the original referent where an internal
  referent needs the copy-twin.

Fix, mechanical: (a) state the chain — each later record's `+d.captured` names the *most
recent* earlier record for that referent, found as the tail of the referent's `-d.captured`
backlink chain (`linked(R, d.captured, NEG)` gives the record pointing at R; follow
`-d.captured` until free — the same read extends the chain across heads and across
sibling-reify copies for free); (b) replace the per-head membership discriminator with the
uniform one — follow `+d.captured` while the target carries `+d.refFor` (a ref-capture record
always does; no referent ever does) — and restate "one hop" as "follow the `+d.captured` chain
to the non-record target" at :1151-1152, :2166 and :3466, scoping the `closed`-frontier claim
(:1152-1153) for cross-head hops (the chained record is another registered head's state-side
cell — admitted, not refused); (c) state that the copy's record→record edges remap through a
map covering recipe records, or regenerate by recipe ordinal (the copy's *k*-th shared-referent
record chains to its *j*-th); (d) pin the sibling-reify order or make the chain-append the
rule so the order stops mattering.

### 2. MINOR — "the clause's `-d.name` holding a single edge" reads the wrong direction

`:1742-1743`: "two cells, never one shared, the clause's `-d.name` holding a single edge".
The clause writes `+d.name` (POS) to its label; its `-d.name` backlink is always empty — what
holds the single edge is the *label's* `-d.name` (to the clause) or equivalently the clause's
`+d.name` (to its own fresh label). An implementer checking the stated side verifies a
vacuous backlink. Fix: "each label's `-d.name` holding a single edge — the clause's `+d.name`
and the record's `+d.name` pointing at different cells".

### 3. MINOR — "not reached by this walk at all" overclaims against the adjacent sentence and the capture case

`:2082-2086`: "named `>E_ops` legs hanging off the *agent cell* are not reached by this walk at
all — `program()` and `agent_cell()` are distinct adapter-bound roots never `+d`-reached from
each other". The roots are distinct, but the legs are not unreached: the walk excludes only
`d.dims` (:1488-1489), and a program-side head's capture edge — a slot link to an agent-`d.vars`
binding cell, the ordinary declared-field capture (:1824-1827) — is exactly the
"runtime-grown edge" the previous sentence says the walk "legitimately reaches beyond the
band" (:2079-2081). Such legs classify state-side either way, so the conclusion (no second
walk needed) is right — the mechanism claim is wrong. Fix: "agent-cell legs are reached only
through capture edges and classify `>E_ops`/state-side regardless, so no agent-rooted second
walk is owed".

### 4. MINOR — "write-only audit metadata" contradicts the two read uses named in the same sentence

`:1186-1190`: "the alias is write-only audit metadata ... its only two uses are the `d.patch`
rediscovery join below and `learn_plan`'s replay dedupe". Both named uses *read* the field —
the join enumerates `alias` key-records to recover `<n>`. The real claim is that the alias is
never a *lookup key* (no name-addressed read resolves by it). Fix: "the alias is audit
metadata, never a lookup key — reachable only by enumerating `d.patch` records".

## Verified accurate (v1.12)

`bindInput` first-slot `+d.grab` and `wingOf`'s absent-head `{}` (`vortex_core.cpp:384-390`,
`:366-368`); `link` displacement (`arena_manifold.cpp:1347-1374`); `Store::scroll`'s
`localScroll` disengage (`store.cpp:1460-1467`, `spool.hpp:19`); `scrollKey` arms
(`scroll.cpp:13-21`); `Store()` mint line (`store.cpp:123-125`); `historyFromSeal`'s internal
`Store` (`publication.cpp:1097`); `d.refFor`/`d.captured` present in all three data-dim lists
(:95, :533, :1017); schema and mechanism restated consistently (:1050-1057 vs :1136-1158);
genesis (e) pairing shape consistent with the schema modulo Finding 1 (:2163-2167); (d)'s
verb-replay hand-off to (e) consistent (:2150-2162).

VERDICT: NOT CONVERGED
