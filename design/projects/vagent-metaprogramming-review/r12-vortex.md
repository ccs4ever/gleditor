# Round 12 critique (vortex role): vagent-metaprogramming.md v1.11

Lane: VortexCore/VM execution semantics, store/spool positions, trail/marks,
handle relocation, promote, MeshActorRuntime invariants. Convergence check on
the v1.11 deltas: (1) the `bindPublishedLocalScroll` stake split (:612-626,
:1468-1476, §10.1 :2625-2629), (2) genesis (b)'s position-space pin
(:2033-2049), (3) the `patch:<n>` durable serial mint (:1156-1169,
:1666-1679, :2176-2179, replay :2068-2073, non-reclaim :1745-1747), (4) the
dedupe consequence (:1171-1188), and R11-B's verify destination
`arena.base()->store()` (:1524-1547, :1578-1600, P16 row, §10.1 :2652-2655);
plus R11-C's ref-capture record (:1047-1050, :1125-1142, :2093-2097), R11-D's
same-name corrections (:1089-1091, :1666-1747), the two-root walk restatement
(:1430-1457, :2012-2016), and the unlearn arm split (:1731-1747).

## Verification of the r11 dispositions

**(1) `bindPublishedLocalScroll` stake split — verified, applied correctly.**
The two halves now name their own mechanisms: `setContentSource` is "the
read-path half, without which every artifact-local span resolves to `""`"
(:614-615 — `Store::read` non-local falls to `resolver.read`, and the remap
makes no span local, `publication.cpp:1049`,`:1086`), while
`bindPublishedLocalScroll(store->addScroll(from))` is "the scroll-identity
half: inert on reads, but without it a later save round-trip hits
`sealPendingMetadataAsCells`' `publishedLocalScroll_ == localScroll` arm and
mints `registerScroll` ops for `self` into the history (`store.cpp:4081-4089`)"
(:622-626). All cites check out: the arm sits at `store.cpp:4081-4089` inside
`sealPendingMetadataAsCells` (`:4020`, called by `save` at `:4094`) — the doc
now correctly names the sealing function rather than `postLoad`; the other
readers are `scrollRegistry`'s `resolvedScrollIds.insert(store_->publishedLocalScroll())`
(`zigzag/manifold.cpp:1503`) and the storeTables round-trip (`publishedLocalScroll`
written `store.cpp:4176`, read back `:4454`). The `self` recovery is exact:
`historyFromSeal` returns only the store (`publication.cpp:1093-1103`,
`selfScrollInHistory` stays file-local at `:1098`), and `addScroll`'s
`sameContentAs` dedup returns the fold-internal id (`store.cpp:1393-1400` —
`externals[0]` → `ScrollId` 1 on a fresh store). §6.6 (:1468-1476) and §10.1
(:2625-2629) carry the same split.

**(2) (b)'s position-space pin — verified.** :2033-2043 now offers the two
honest encodings: `produces` `MicroversionId`s "resolved to positions on the
*received* fold (`opsSpool` keys records by id, `store.cpp:3841`;
`indexOf`/`idOf` translate)" — `opsSpool.contains(record.produces)` at
`store.cpp:3840`, `indexOf`/`idOf` at `segmented_ops_spool.hpp:279-287`,
`cellRefOf` wraps it `store.hpp:677-681` — "or, equivalently, as positions *in
the canonical post-fold order*, which requires the stated invariant that the
image's promote appends formed a linear `parent` chain". The invariant is
sufficient and correctly argued: `adoptOpRecords` is a topo-sort drained by a
min-`produces` heap (`store.cpp:3836-3940`, `cmp` at `:3909-3913`), and a
strictly linear parent chain forces the unique topological extension — extra
deps (`source`/`context`/structure fields, `:3849-3876`) all point backwards,
so sealed order == fold order. The skew argument is right: a branched band
permutes, fold-vs-fold of the same segment still compares equal under (a), and
recorded positions would point at wrong cells — `produces` ids are
permutation-stable. The kind source is now stated (:2047-2049): recorded
`instruction.opcode` (the `mesh_evaluator.cpp:658-663` precedent, verified —
`setOpcodeKind(cell, instruction.opcode)` inside the `program.instructions`
loop) or the recorded cell's own `asInt64`, safe for a *recorded* cell since
`getOpcodeKind`'s fallback decodes it (`vortex_vm.cpp:107-112`).

**(3) `patch:<n>` durable serial off `d.vars` — verified, internally
consistent.** The mint rule (:1156-1161): "`<n>` the patch's **durable
monotone serial**, drawn from the same per-agent `d.vars` counter `reify`'s
`<serial>` already rides (§6.7), never the rank position: `unlearn` drops and
replay-drops renumber the rank, and a recycled ordinal would re-mint a live
`patch:<n>` and misbind every descriptor recording it". The `reify` precedent
exists (:2176-2178 — "a *durable per-agent counter* on `d.vars`, not a
per-incarnation one") and the shared counter is safe: `reify:`/`patch:` label
spaces are prefix-disjoint, so interleaved serials cannot collide. Non-reclaim
is stated (:1745-1747 — "the `d.vars` serial stays spent ... replay-dropped
patches likewise leave their serials consumed"), replay transports the label
verbatim (:2068-2073 — "(d) ... minting no patch cells, so no serial is
consumed ... the copy's `+d.name` coming **verbatim off the patch record** ...
it never re-derives `<n>`"), and the alias is pinned write-only
(:1162-1166 — never a lookup key; rediscovery is `clauses_of` + the `patch:`
prefix or `plan_named("patch:<n>")` with the ordinal read off `d.patch`).
`plan_named("patch:<n>")` resolves because the first-`:` split's module arm
fails on `patch` — reserved at module declaration (:2171-2175) — so the whole
string falls through to the verbatim `+d.name` match: the reservation is
load-bearing and the chain is closed.

**(4) Dedupe consequence — verified as adopted, with a residual gap (finding
2).** :1173-1180 now states it: the dedupe fires "only when the alias spells
the target's stored `+d.name` verbatim — a *qualified* spelling against a
declared clause — so `learn_plan("Feed", "demo:Forager.travel", …)` unlinks
the declared clause and installs `patch:<n>` — the qualified name then
resolves to nothing, and name-preserving replacement of a declared clause goes
through `learn_where`/`learn_body`, which keep the named clause".

**(5) R11-B verify destination — verified; `promote`'s persistent-ref map now
has a defined target band.** The chain: `checkpointView` yields `const
ArenaManifold&` → `arena.base()` → `const Manifold*` (`arena_manifold.hpp:268`)
→ `->store()` — `Manifold::store()` is `const noexcept` returning non-const
`Store*` (`manifold.hpp:486`, verified verbatim) — the mutable reach, while
`ArenaManifold::store()` itself returns `const Store*` (`arena_manifold.hpp:269-271`,
"const-only", verified). Under the verify arm `publishValue` (signature
`Store&`, `mesh_value.hpp:69-74`) runs against the runtime's rebuilt base
store (:1579-1582); under the supply arm the adapter's seeded store *is* that
store. Either way the publish destination is the arena's own base store — and
that is precisely the store whose spool positions the arena's persistent refs
name: shadow cells carry the base `birthOp` untouched
(`arena_manifold.cpp:1489-1490`), so `promote`'s verbatim map
(`real.emplace(arena, arena)`, `arena_manifold.cpp:1867-1868`) writes
positions that exist in the destination by construction. The dangling-write
exposure the r11 ruling named is closed. The kept-in-step twin is correctly
named owed surface (:1543-1547): `adoptOpRecords` takes already-decoded
records into `this` (`store.cpp:3836`), `applyOpsSegment` (the `\x7fXSO\x01`
gate + scroll-table remap) is file-local inside `historyFromSeal`
(`publication.cpp:1030+`,`:1093-1103`) — nothing applies a sealed *segment*
into a caller-held store. The `scrollKey` comparand leg (:1537-1539) is sound
in-domain: every published scroll is keyed (`restorePublication` refuses
`!identity`, `publication.cpp:1124-1126`), so `scrollKey`
(`scroll.hpp:333`, returns `optional<string>`) is non-null for every scroll a
lineage band can name — see the nits for the residual deref nitpick.

## New findings (v1.11 deltas only)

### 1. MAJOR — the ref-capture record's `+d.grab → slot` evicts the slot's `-d.grab` wing backlink, decapitating the op's input wing

**Location:** :1047-1049 (recipe diagram) and :1129-1132 (§6.1 recipe text) —
"`+d.grab → the same slot cell` the ref-record names by `+d.ref` (a second
`+d.ref` edge cannot correlate them — the slot's `-d.ref` backlink is
single-occupancy and the later link would evict the first, M14's rule)".

**Problem.** The M14 reasoning is right and the chosen dim is the *worst
possible* one under it: a ref cell is an emitter-minted int64 cell naming
another cell — the canonical instance is an op's *wing slot*
(`bindInput` mints `makeScalarCell(int64(operand))` and links
`opcode +d.grab → slot`, `vortex_core.cpp:380-396`; `bindOutput` the same on
`-d.grab`, `:398-414`). For the first input slot, the slot's `-d.grab`
backlink **is occupied — by the opcode itself**. `link()` clears the target's
displaced `-d` neighbour: `displacedIt = theirs->neighbor(-dir)` then
`setOneSide(*other, dim, dir, noCell)` — `arena_manifold.cpp:1352-1373`, the
exact eviction code the doc cites at :1038-1039. Writing
`record +d.grab → slot` therefore clears `opcode +d.grab → slot` — and since
`wingOf` enters the input wing through `linked(opcode, dims_.grab, POS)`
(`vortex_core.cpp:366-369`), clearing the head edge makes `inputsOf` return
**{}**: the op loses its *entire* input wing, not just the slot. The
severance happens twice: at emission, when the recipe is written on the live
fragment (the fragment is corrupt before any `run`/`graft`), and again at
`reify`, when the regenerated record is linked onto the copy's fresh wing
slots after they are bound. The occupancy the doc invoked against `+d.ref`
(the slot's single-occupancy `-d.ref` backlink) applies identically to
`-d.grab` — except `-d.grab` is *already* occupied on the primary target
class. Only chained input slots (`-d.step`) and output-wing slots
(`+d.grab`→opcode) have a free `-d.grab`.

**Fix.** Correlation needs a dim whose backlink is free on every slot — the
house style's own answer: more dimensions, not overloaded ones. Mint a
dedicated `d.refslot`/`d.capture-slot` correlation dim
(`record +d.refslot → slot`), or drop the edge entirely and correlate
positionally — the recipe's ref-capture records and the `+d.refs` ref-records
are both `+d.step`-ordered chains, and "one per ref-record" already implies
the k-th pairing. Either way, state the occupancy check the choice rests on
("the slot's `-d.<dim>` is free because wing membership sits on
`-d.grab`/`-d.step`"), the same shape of argument the paragraph already makes
for `+d.ref`.

### 2. MINOR — the dedupe paragraph contradicts itself on grafted clauses, and replay re-fire is unspecified

**Location:** :1173-1188.

**Problem.** Two gaps in the new consequence text. (a) "under the mint the
alias can only hit a **declared or grafted** clause" (:1174) and "an
unqualified alias always *appends* under its own `patch:<n>`" (:1179-1180)
cannot both hold: a `graft_*`-installed clause holds the `name` argument on
its `+d.name` verbatim (:1155) — including *unqualified* spellings like
`"near_water"` — so `learn_plan("Feed", "near_water", …)` verbatim-matches the
grafted clause and the dedupe unlinks it; that is a replace, not an append.
The "always appends" claim survives only for aliases spelling no live
`+d.name`. (b) Whether genesis replay re-fires the alias dedupe is unstated,
and it decides the durable semantics: the unlink is a trailed arena write with
no patch-recorded edge — `d.patch` replay "re-runs each graft" (:2122), and
the patch carries the alias verbatim — so if replay re-dedupes, the declared
clause stays dead post-resume (durable replacement); if it does not, the image
folds the declared clause back at `1..E_ops` and `patch:<n>` replays beside
it, and "the qualified name then resolves to nothing" (:1177-1178) holds only
in-session — the same resurrection shape the doc embraces for declared
handlers at :1728-1730 but does not name here.

**Fix.** Split (a): "the dedupe fires on any verbatim hit — qualified or not —
against a declared *or grafted* clause's stored `+d.name`; an alias spelling
no live `+d.name` appends". Then pin (b): either "replay re-runs the dedupe,
so a declared-clause unlink is durable" or "the unlink is session-scoped —
the image restores the declared clause at resume and `patch:<n>` replays
beside it, like the declared-handler shadow rule".

### 3. MINOR — "no agent-rooted walk can reach a `≤E_ops` cell" fails for a program-side `agent_cell()` root

**Location:** :1453-1457 — "the sideness test needs no such walk —
agent-reachable cells classify state-side regardless, since V12's
state→program-skeleton link bar means no agent-rooted walk can reach a
`≤E_ops` cell" (restated :2015-2016).

**Problem.** V12 bars *state→program-skeleton* links; a program-side
`agent_cell()` — a case the document itself holds open ("its own root exempt:
a program-side `agent_cell()` cannot prune the durable set", :1608-1609) —
has *program→program* legs, which the bar does not touch. A folded image's
`+d.belief → predicate → +d.clause → clause` chains (spec:
`vagent-agent-language.md:478,480,508-509`) sit at `≤E_ops` and are reached by
an agent-rooted outbound walk. The conclusion survives because the
discriminator is positional (a `≤E_ops` ref classifies program-side wherever
met), but the stated mechanism is false in exactly the exempt case, and an
implementer who encodes "agent-reachable ⇒ state-side" as a shortcut
misclassifies the image's own agent legs.

**Fix.** Scope the claim: "nothing an agent-rooted walk reaches *through a
link beyond its root* can classify program-side — the V12 bar — and when
`agent_cell()` is itself `≤E_ops` (§6.6's exempt case) its image legs classify
by ref position like every other cell."

### 4. MINOR — "share the `patch:<n>` `+d.name`" invites a structurally impossible reading

**Location:** :1674-1676 — "Patch and installed clause share the `patch:<n>`
`+d.name` intentionally".

**Problem.** Read as cell-sharing it cannot be done: the label cell's
`-d.name` backlink is single-occupancy like every dim neighbour, so
`patch +d.name → L` after `clause +d.name → L` evicts the clause's label edge
— `plan_named("patch:<n>")` then misses the clause entirely. Only the
*string* can be shared (two label cells holding `patch:<n>`, or one label
cell plus content-copy). The next clause ("the sharing cannot collide because
`plan_named`'s space is clause-rank cells") already reasons about content,
so intent is clear — but the sentence sits beside text that *is* about link
occupancy and will be read in that register.

**Fix.** "...carry the same `patch:<n>` string on their own `+d.name` label
cells intentionally — the patch's label names the clause it installed".

### 5. MINOR — the target-descriptor arm enumeration omits named *grafted* clause targets

**Location:** :1700-1708 — "the convention has two arms: the **qualified**
`[<module>:][<Agent>.]<member>` spelling for program-original clause and
member targets ..., and the clause's **verbatim `+d.name`** — the `patch:<n>`
the learning verb minted — for learned-clause targets".

**Problem.** `learn_where`/`learn_body` take a clause *cell*; a named grafted
clause (session-only `graft_*` install, `+d.name` = user name verbatim) is a
legitimate, non-anonymous target — the :1720 refusal covers only *anonymous*
grafted clauses, so a named grafted clause is recordable — and neither arm
names it. The qualified arm cannot apply (a grafted clause's name may be
unqualified); verbatim `+d.name` is the only viable arm, but the text scopes
it "for learned-clause targets". An implementer must guess.

**Fix.** Extend the second arm: "the clause's verbatim `+d.name` — the
`patch:<n>` mint for a learned clause, the user `name` for a named grafted
clause — for non-program-original clause targets".

## Items verified clean in the v1.11 deltas

- **`d.contract`/`d.source`/`d.belief` anchoring** — `preconditionsOf`/
  `postconditionsOf` take `CellRef opcode` and walk `-d.contract`/`+d.contract`
  off op chains (`vortex_core.cpp:464-470`, used at `vortex_stdlib.cpp:604`,
  `:629` — doc cites :603,:628, the comment lines; close enough), so
  `d.contract` stays program-side; spec confirms `d.source` is clause→peer and
  `d.belief` is agent cell→predicates (`vagent-agent-language.md:478-480`),
  matching the move.
- **`unlearn` symbol arm** — `exportSymbol` mints `makeCell(symbolName)` +
  `+d.values`→entry + rank link (`vortex_core.cpp:119-127`); `-d.vars` back
  to the rank head reaches the module cell (`rankTail(moduleCell, vars)`
  chain, `:125`), whose content supplies `module:` — the composition is exact.
- **`unlearn` clause arm / LIFO scoping** — consistent: verbatim transport
  makes `patch:<n>` re-resolvable post-resume (`plan_named("patch:<n>")`), and
  stacked `learn_where`/`learn_body` re-points on one learned clause share the
  recorded `patch:<n>` target — LIFO is undo order, correctly stated.
- **`learn_method` corollary** (:1797-1805) — verified: `exportSymbol`'s
  third arg is `entryOp` — a declared `def`'s `+d.values` names an op, so
  only `learn_method`-installed (`+d.values`→head) symbols satisfy the
  head-valued landing a qualified `run`-role record needs.
- **`-d.ref` single-occupancy justification** (:1130-1132) — the
  displacement code (`arena_manifold.cpp:1347-1373`) confirms the claim's
  mechanism; the choice it motivates is finding 1.
- **Replay `+d.name` transport** (:2068-2073) — the patch record's own
  `+d.name` is the durable source, so the verbatim copy is defined.
- **`checkpoint.store` plumbing** — `!checkpoint.store` guard at
  `mesh_actor.cpp:188` (verified — the `|| !checkpoint.store ||
  !checkpoint.manifold` line inside the `resume` validity check),
  `userPermascrollPtr()` read at `:261`, `WorkerContext(checkpoint->manifold
  .get())` + `setDefaultSpanReader(checkpoint->store.get())` at `:271-273`,
  `unique_ptr` `mesh_value.hpp:97-104` — all accurate.
- **`ensureDimension` name arm** — `dimensionNamed` resolves base dims and
  `arenaDims_` before the mint, `arena_manifold.cpp:78-105` — correct for the
  program-side referent resolution.
- **`setOpcodeKind` cite** — `vortex_vm.cpp:98-100` is correct now
  (signature :98, body :99); the r11 nit is resolved.

## Nits (non-blocking)

- :2035 — "`opsSpool` keys records by id, `store.cpp:3841`" — the
  `opsSpool.contains(record.produces)` call is at `store.cpp:3840` (`:3841`
  is the `continue`).
- :637, :1483 — the `Store()` `UserPermascroll` mint is the *initializer* at
  `store.cpp:124` (`:123` is the signature; `:125` is `chronofilade_`). The
  r11-disposition "correction" `:123-124`→`:123` rested on a misread — `:124`
  *is* the mint; restore `:123-124` or cite `:124`.
- :1538 — `*store->scroll(span.scroll)`: `Store::scroll` returns
  `cpp26::optional<const Scroll&>` with `nullopt` for `localScroll`/out of
  range (`store.cpp:1461-1468`) — the comparand needs the `.and_then` shape,
  and `scrollKey` is `nullopt` for anonymous scrolls (vacuous in-domain since
  `pub.scrolls` are all keyed, but worth one clause).
- :1537 vs (a) :1992-1994 — (a) still says the comparand is "op kind plus
  non-scroll fields"; the verify arm adds the `scrollKey` leg. One token in
  (a) ("non-scroll fields, scrolls by key") keeps the two statements equal.
- :1091 — `label_of`'s `[Cell]` indexing caveat now says "the first two
  paths yield `[Cell]`", but `clauses_of` yields `[Cell]` too — "each path"
  or "the first three".
- :1585 — "the seeded store's positions above `E_ops` are stale
  image-lineage": when present those are stale *state* ops of the prior
  incarnation, and an image-only seeded store has none — "stale lineage
  positions" covers both.
- :1449 — "the spec's other agent ranks" sweeps loosely: `d.source`/
  `d.extends` sit on the spec's dim list but are clause-/type-anchored
  program side (`vagent-agent-language.md:480-481`) — the criterion is
  agent-cell anchoring, not the spec list.

## Summary for the orchestrator

All five assigned verifications pass: the hydration pair's stakes are split
exactly on the read-path/scroll-identity line with every cite checking out
(`sealPendingMetadataAsCells` now correctly named); (b) records `produces`
ids or pins the linear-`parent`-chain invariant that makes canonical-order
positions honest; `patch:<n>` is a durable monotone `d.vars` serial with
non-reclaim and verbatim replay transport closed end-to-end; the dedupe
consequence names the qualified-alias unlink; and R11-B's
`arena.base()->store()` destination is real, mutable, and gives `promote`'s
verbatim persistent-ref map a defined band — the exposure is closed. One new
MAJOR in the R11-C fix: the ref-capture record correlates slot↔record over
`+d.grab`, whose backlink is *already occupied* by the op on every first
input-wing slot — `link()`'s `displacedIt` clearing decapitates the whole
input wing, at emission and again on reify; the M14 argument invoked to
reject `+d.ref` convicts `+d.grab`. Four MINORs: the dedupe paragraph's
grafted-clause self-contradiction plus unspecified replay re-fire; the
agent-walk `≤E_ops` claim's program-side-root hole; the `+d.name`
cell-sharing phrasing; and the unaddressed named-grafted target arm. Counts:
**0 BLOCKING / 1 MAJOR / 5 MINOR / 7 nits**.

VERDICT: NOT CONVERGED
