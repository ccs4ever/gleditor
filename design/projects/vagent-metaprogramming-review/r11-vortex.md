# Round 11 critique (vortex role): vagent-metaprogramming.md v1.10

Lane: VortexCore/VM execution semantics, store/spool positions, trail/marks,
handle relocation, promote, MeshActorRuntime invariants. Convergence check on
the v1.10 deltas: (a) genesis (b)'s recorded instruction enumeration
(:1921-1944, §6.5 :1332-1342, P15 row :2563), (b) the span-source ceremony
(`setContentSource`+`bindPublishedLocalScroll`, :608-620, :1415-1420, §10.1
:2489-2500), (c) capture-endpoint internal/external split (:1965-1979), (d) the
order-vs-order comparand (:1877-1889, :1466-1470), (e) the `WorkerContext`
1-arg/`setDefaultSpanReader` restatement (:1458-1464, :1785-1794, :2546-2555),
plus the `patch:<n>` minting rewrite and the minor batch.

## Verification of the r10 dispositions

**R10-B — verified end-to-end.** Genesis (b) now names the discriminator: the
op-cell set is "exactly the recorded instruction enumeration" the artifact
writes at emission on the `programRef` bind record `E_ops` rides (:1924-1928),
applied by `setOpcodeKind` to the folded cells at those positions
(`vortex_vm.cpp:97-99` — `opcodeMap_[opCell] = op`). The precedent checks out:
`mesh_evaluator.cpp:658-663` iterates `program.instructions` and calls
`context.vm.setOpcodeKind(cell, instruction.opcode)` — ops known by
construction, never discovered by value. Both wrong shapes are argued
correctly: an `asInt64 ≤ Linked` sweep aliases `dimConstant` operand cells
(`getOpcodeKind`'s fallback at `vortex_vm.cpp:106-112`; `dimConstant` mints
`literal(int64(findOrMintDimension(name)))`, `vagent/compiler.cpp:769-772` —
genesis-era dim positions sit under the 48-enum ceiling), and `mintOpcode`
mints a fresh ephemeral cell (`vortex_vm.cpp:73-82`), registering twins, not
the folded cells. §6.5 :1336-1345 carries the same statement ("over exactly the
recorded instruction enumeration", "the `asInt64 ≤ Linked` value scan §6.6's
step (b) rules out"), the P15 row (:2563) matches, and §10.1's comment
(:2525-2526) records "the recorded image extent and instruction enumeration
genesis (a)/(b) assert" — the bind record now carries both. The dependency
restatement is exact: `opcodeMap_` is per-`WorkerContext`, populated by
`VortexStdLib`'s `mintOpcode`s during construction (`vortex_stdlib.cpp:1436`,
`:1504`+), holding stdlib ops but "none of the folded image's op cells" —
replacing v1.9's wrong "empty".

**R10-C — verified, the dead-read chain is real and the fix lands.** The pair
is `restorePublication`'s own: `history->setContentSource(&source)` +
`history->bindPublishedLocalScroll(self)` at `publication.cpp:1129-1131`
(`self = history->addScroll(local->second)` — the publication's history scroll,
same role as `historyFromSeal`'s `from`). `historyFromSeal`
(`publication.cpp:1093-1103`, decl `publication.hpp:605-612`) does the bare
fold: `std::make_unique<Store>()` at :1097, `addScroll(from)`, per-segment
`applyOpsSegment` — no source, no bind. The failure mechanism verifies hop by
hop: the remap makes *no* span local (`remap.emplace(localScroll,
selfScrollInHistory)` at `publication.cpp:1049`, `record.op.span.scroll =
at->second` at :1086) so every artifact span lands on an external scroll;
`Store::read` non-local falls to `resolver.read(*which, span)`
(`store.cpp:1588`) and `Resolver` documents "may be null, in which case
external spans simply do not resolve" (`resolver.hpp:252-255`) — `""`; and
`initGenesis`'s name path is exactly `mintNamedDimension` →
`baseDimensionNamed` (`vortex_core.cpp:18`,`:73`,`:349-351`) →
`arena_.base()->dimensionNamed(name)` → `dimensionNamed(name, *store_)`
(`manifold.cpp:824-828`) → `textOf(dim, reader)` → `reader.read(span)`
(`manifold.cpp:830-833`) → `""` → twins minted. The doc's claim
("`initGenesis` mints ephemeral twin dims and every name-resolution path
dead-reads", :1419-1420) is accurate — and understates it, since the twin
dims fork dim identity for every subsequent `core.dims()` consumer.
Companion claims verified: `Store()` mints a fresh `UserPermascroll`
(`store.cpp:123-124`); `documentId_` is `Store`-ctor state (`store.hpp:133`,
the field at :1433); `applyOpsSegment` is file-local (anonymous namespace
`publication.cpp:1023-1091`), so the adopt-id arm (`Store(userPermascroll,
pub.storeId)`, `publication.cpp:1127-1128`) is correctly named unmechanized —
a signed `Publication` consumer, not a segments one. The `resume` transport
correction is exact: `resume` takes `ReceivedValue checkpoint` by value
(`mesh_actor.cpp:182-186`), reads `checkpoint.store->userPermascrollPtr()` at
:261 into `receiveValue`, keeps `verified` not the param — the caller store is
dropped; `checkpoint.store` is a `unique_ptr` (`mesh_value.hpp:97-104`).

**R10-D — verified.** :1968-1974 now splits the arms exactly as ruled: the
ref-record's `+d.ref` slot "identifies *which* cell's referent is captured"
while the endpoint "rides the *recipe* record (the recipe tuple's `+d.values`,
§6.1 — a cell, never a census integer)"; an *internal* referent "is rebound
through the old→new reify map to its copy twin", an *external*/capture
referent "is the fold cell at the promoted position", and the
`int64(endpoint)` == `*handleTarget` equality is scoped "for the external arm
only". Consistent with the unified `+d.refs` schema (:1023-1032 — "`+d.ref`,
never `+d.values`") and §6.1's recipe convention (:1109-1115).

**R10-E — verified.** :1880-1888 pins the comparand: "the adapter decodes its
adopted lineage store's own `1..E_ops` band — the same op records
`adoptOpRecords` folded it into, read back in canonical order — and asserts
the received store's decoded `1..E_ops` records equal it pairwise", canonical
post-remap `OpRecord`/`CompactOpNode` fields, never bytes-vs-records. Both
legs of the determinism claim hold: the `ScrollId` remap rewrites only
`.span.scroll` (`publication.cpp:1048-1086`) and `adoptOpRecords` is a
deterministic topo-sort with a `produces`-min priority-queue tie-break
(`store.cpp:3836-3940`, heap at :3909-3913). The parallel spawn-side text
(:1466-1470) and §10.1 match.

**WorkerContext call shape — verified.** The two-arg form is
`worker_context.hpp:52-54` exactly (`explicit WorkerContext(const Manifold
*baseManifold = nullptr, const SpanReader *reader = nullptr) :
arena(baseManifold), core(*arena.setDefaultSpanReader(reader)), ...`); resume
passes `base` alone (`std::make_unique<WorkerContext>(checkpoint->manifold
.get())`, `mesh_actor.cpp:271`) and installs the reader post-construction
(`arena.setDefaultSpanReader(checkpoint->store.get())`, :273); the spawn arm
supplying both at construction is equivalent-in-effect — the reader install
precedes the `VortexCore` ctor (member-init order), and `initGenesis`'s own
name path goes through the base manifold's `store_` (= the seeded store,
`rebuildManifold` calls `folded.setStore(this)`, `store.cpp:511`,`:532`), so
hydration-before-base-injection is the ordering that matters and the doc has
it right. `ArenaManifold` holds `base_` as a raw `const Manifold*`
(`arena_manifold.hpp:752`), so the pin is load-bearing; `attach(Space)`
installs `base_`/`store_` only when unset (`arena_manifold.cpp:278-283`) and
cannot re-run `initGenesis` (`vortex_core.cpp:14`,`:29-70`; `home_`/`dims_`/
`initGenesis` private, `vortex_core.hpp:282-287`).

**Minor batch — all verified.** `opcodeMap_` correctly removed from
`initGenesis`'s freeze set (private VM state, `vortex_vm.hpp:186`, populated
by stdlib mints). The two-root walk vocabulary (:1389-1405) is coherent:
`+d.code` moved to agent-cell legs keeps clause-side `+d.code` memo chains
unwalked by the program-rooted pass — they classify nothing and die behind
the program-side prune at `promote` (`arena_manifold.cpp:1755-1757`), which
is the stated intent (:1531-1535); ingress restated as classification in both
arms (:1904-1911). `label_of`'s `cells"$a/+d.code"` off `agent_cell()` is the
right anchor — `d.code` is the agent-cell rank, `##` is arena `home`. The
run-slot wording (:1687-1698) is consistent: `None`-observable unwritten
slot, no sentinel, fail-until-resume vs fail-forever split. R10-A is
internally consistent: `patch:<n>` minted by the verb on the installed
clause's `+d.name` (:1588-1593), the user `name` demoted to a patch-record
alias, §9.3's example passes `"near_water"` (:2383-2401), the descriptor's
learned-clause arm records the verbatim `patch:<n>` (`plan_named` matches
clause `+d.name`s verbatim), and "`patch:` is a *clause*-label prefix ...
never a head label" (:1740-1741) holds under the head-layout shape
restriction (:1759-1763). `unlearn`'s stated limit is consistent with the
:1643-1646 mechanism (arg cell → `+d.name`/content → match patch *target*
names; event/goal targets are shared names, so the latest pops first). Cite
fixes verified: `setLink` `arena_manifold.cpp:2041`, `real.emplace` :1868,
`historyFromSeal` range :1093-1103, `mesh_actor.cpp:271-273` —
`mesh_actor.cpp:268`'s `actor->sourceLease` role also checks out.

## New findings (v1.10 deltas only)

### 1. MINOR — `bindPublishedLocalScroll` is not on the read path; the "without which" claim belongs to `setContentSource` alone

**Location:** :608-612 (§4.5) and :1416-1420 (§6.6) — "`setContentSource` on
the adapter's content source plus `bindPublishedLocalScroll(self)` —
`restorePublication`'s own pair at `publication.cpp:1129-1131`, without which
every artifact-local span — all `d.*` dim names, `+d.name`/`src:` labels,
symbol names — resolves to `""`".

**Problem.** `publishedLocalScroll_` is never consulted by `Store::read`,
`Store::resolve`, `containerFor`, or `Resolver` — its consumers are
`scrollRegistry`'s resolved-scroll walk (`manifold.cpp:1503`),
`reader_link_packages.cpp:171`, the `storeTables` round-trip
(`store.cpp:4176`,`:4454`), `postLoad`'s scroll registration
(`store.cpp:4081-4089`), and the imported-tables byte verification
(`store.cpp:4473-4490`) — none of which the
`historyFromSeal`→`publishValue`/`sealLocalSpool` path touches (`publish`
takes `localSealedAs`, not `publishedLocalScroll_`, `publication.cpp:1280+`).
Byte resolution is gated by `setContentSource` alone (`Resolver`'s nullable
source, `resolver.hpp:252-255`). `bindPublishedLocalScroll` *is* load-bearing
for the agent store, but for a different reason than stated: it keeps the
published-scroll identity on the lineage — and without it a store that ever
save/load round-trips hits `postLoad`'s
`publishedLocalScroll_ == localScroll` arm and mints `registerScroll` ops for
`self` *into the history* (`store.cpp:4081-4089`), polluting the lineage
spool. The pair is the right prescription; the stated mechanism is half
wrong.

**Fix.** Split the roles: "`setContentSource` makes the remapped spans
resolve (`Resolver`'s nullable source); `bindPublishedLocalScroll(self)`
keeps the artifact's history scroll bound as the lineage's published root —
inert on reads but required so a later save/load round-trip does not
`postLoad`-register `self` as a foreign deployment and mint `registerScroll`
ops into the history (`store.cpp:4081-4089`)."

### 2. MINOR — the instruction enumeration records band *positions*; adoption can permute a non-linear op DAG and neither (a) nor (b) notices

**Location:** :1925-1928 — "the artifact records which band positions are
its op cells at emission, on the same `programRef` bind record `E_ops`
rides".

**Problem.** A band position is an `opsSpool` index — the folded cell's
`birthOp` — and `adoptOpRecords` topologically re-sorts records with a
`produces`-hash tie-break (`store.cpp:3836-3940`). Positions recorded at
emission equal adopted positions only because a parent-chained op stream
emitted sequentially has a *unique* topological extension. If an artifact's
op DAG is ever non-linear (a branch — two records sharing a parent), adoption
may legitimately permute the band; then (a) still passes — it compares
adopted-vs-received, both permuted identically — while (b) registers the
cells *at the recorded positions*, which are no longer the op cells. That is
the exact misregistration R10-B existed to prevent: data cells enter P15's
refusal and real op cells stay surgically writable. `MicroversionId`
`produces` identities are permutation-stable (`opsSpool.contains(record
.produces)` keys the spool by id, `store.cpp:3841`; `indexOf`/`idOf` resolve
either way, `store.hpp:677-681`), so recording them costs nothing extra.

**Fix.** "…the artifact records its op cells' *op identities* at emission —
the `produces` MicroversionIds, resolved to band positions through
`opsSpool.indexOf` at the bind — or, equivalently, band positions recorded
with the stated invariant that the artifact's op stream is parent-chained
(emission order is the unique topological order, so adoption cannot permute
it). Positions alone rely on an invariant nobody has named."

### 3. MINOR — `patch:<n>`'s ordinal allocation is unspecified; a recycled ordinal silently retargets recorded names

**Location:** :1130-1133 ("`<n>` = the `d.patch` ordinal the call creates"),
:1589-1590, glossary :136.

**Problem.** `patch:<n>` labels are recorded *verbatim* as target names in
other patches' descriptors (the learned-clause arm, :1617-1621) and resolved
by `plan_named`'s verbatim `+d.name` match — uniqueness over the *lineage* is
load-bearing. "The `d.patch` ordinal the call creates" does not say how the
ordinal is allocated. `maxOrdinal(live rank)+1` or rank-size arithmetic
recycles: `unlearn` drops dead patches from the rank (:1584-1585, :1995-2000),
freeing their ordinals, and a later `learn_*` then mints a `patch:<n>` that a
still-live patch's recorded target name already spells — genesis replay would
resolve that name to the *new* clause and install/re-point the wrong target
silently. The honest allocation is a monotone counter on the lineage
(dead patches leave ordinal gaps), never a rank position.

**Fix.** One clause: "the ordinal is monotone for the agent's lineage — each
mint takes the highest ordinal ever issued plus one, so a dropped patch's
`<n>` is never reissued; rank position is not the ordinal."

### 4. MINOR — a qualified-spelling `name` alias under `learn_*` silently deletes the named clause the dedupe unlinked

**Location:** :1133-1141 — "`learn_plan` matching its alias verbatim against
clause `+d.name`s, so a learned clause's minted `patch:<n>` is never re-hit
and successive learns append rather than self-replace."

**Problem.** The dedupe matches the user `name` (alias) against `+d.name`
labels verbatim, so it *does* fire when the alias spells an existing
qualified name: `learn_plan("Feed", "demo:Forager.travel", …)` unlinks the
declared clause and installs `patch:<n>` — the qualified name now resolves to
nothing (`plan_named` matches `+d.name`s; `tried`/intentions record plans by
qualified name, :1126-1128). Under verb-minted labels, `learn_plan` can never
be a name-preserving replacement — that role belongs to
`learn_where`/`learn_body`, whose re-point keeps the clause and its
`+d.name`. The text states the mechanism but not the consequence; an
implementer reading "dedupe by name" builds a replace that deletes the name.

**Fix.** One clause: "a `name` alias that *does* collide with a live
`+d.name` (only a qualified spelling can) unlinks that clause and installs
`patch:<n>` — the qualified name goes unresolved, so name-preserving
replacement of a declared clause goes through `learn_where`/`learn_body`,
which keep the named clause."

## Items verified clean in the v1.10 deltas

- **P15/P16 rows** — the funnel sites, exemptions and ceremony match the body;
  `writeValue`/`splice` already route through `masterOf`
  (`vortex_core.cpp:196-210`,`:344`), so `opcodeMap_.contains(masterOf(cell))`
  composes; `runPipeline` writes `:337`,`:343-346`, memo-hit `:1269-1273`,
  generic writeback `:1287-1294`, `Call` frame `makeCell`+`setValueBits`
  `:858-870`, `setCursorOpcode` `:129-143`, `toInt(inputs[0])` `:855`.
- **`opcodeMap_` spawn/resume symmetry** — the map holds stdlib mints at
  both binds but no image ops; the pass is needed at both exactly as stated.
- **`store()` plumbing** — `ArenaManifold::store()` public falling to
  `base_->store()` (`arena_manifold.hpp:269-272`); `Manifold::store()` const
  returning non-const `Store*` (`manifold.hpp:486`) — the verify arm's
  de-facto reachability claim is accurate.
- **`attach(Space)`** — sets `base_`/`store_` only when unset and defaults
  `space.reader` to `space.store` (`arena_manifold.cpp:274-287`) — the
  no-re-genesis argument stands.
- **Genesis order** — (a) precedes (b), so recorded positions only ever
  apply to a comparand-verified band on the resume arm (spawn binds by
  construction; the self-comparand degenerates harmlessly).
- **Folded-cell lookup** — `birthOp == this CellRef` (`manifold.hpp:295`),
  assigned `= opIndex` verbatim (`manifold.cpp:427-442`), so position →
  folded cell is a direct ref lookup.

## Nits (non-blocking)

- :1928-1934 — the enumeration records positions but never says where the
  *kind* comes from for `setOpcodeKind(cell, op)`: the folded cell's own
  `asInt64` (safe per *recorded* cell — the sweep was the wrong shape, not
  the decode) or recorded `instruction.opcode`-style kinds like the
  `mesh_evaluator.cpp:663` precedent. One clause.
- :1404-1405 — "whose cells all classify state-side as `>E_ops`" conflates
  the arms: agent-side cells are *ephemeral*, classified state-side by the
  ephemeral arm; `>E_ops` names only the persistent arm.
- :1141-1145 — "same-named learned clauses" reads as `+d.name`-sameness,
  which `patch:<n>`'s uniqueness makes impossible; the shared name is the
  patch *target* name (the event/goal the twin learns recorded) — the
  :1643-1646 mechanism bears it, the phrase should say it.
- :1339 — "`mintOpcode` is the other wrong shape — the `asInt64 ≤ Linked`
  value scan §6.6's step (b) rules out being the first" parses badly;
  reorder to "…is the other wrong shape; §6.6's step (b) rules out the
  first, the `asInt64 ≤ Linked` value scan:".
- :1338 — `setOpcodeKind` is `vortex_vm.cpp:97-99` (signature :97), not
  :98-100; `documentId_` sits at `store.hpp:1433`, not :1435.
- :3098-3099 — the v1.9 changelog entry still carries the pre-R10-D phrasing
  ("the identical bits `*handleTarget` carries" without the external-arm
  qualifier); historical, but a one-word scoping keeps the record honest.

## Summary for the orchestrator

All five r10 dispositions verify against the code: (b)'s recorded
enumeration names the right discriminator with both wrong shapes ruled out;
the span-source pair is `restorePublication`'s own and the dead-read chain is
real hop-by-hop; the capture endpoint splits internal/external correctly with
`*handleTarget` scoped to external only; the comparand is the adopted store's
post-fold band order-vs-order; and the `WorkerContext` call shape is now
accurate. No BLOCKING and no MAJOR from this lane. Four MINORs: the
`bindPublishedLocalScroll` dead-read attribution (reads are gated by
`setContentSource`; the bind holds scroll identity for registry/postLoad),
the position-keyed enumeration's unstated linearity invariant (record
`produces` identities or name the invariant), the unspecified `patch:<n>`
ordinal allocation (recycling retargets recorded names), and the unstated
consequence of a `+d.name`-colliding alias under `learn_*` (the dedupe
unlinks the named clause and installs `patch:<n>`, leaving the qualified
name dead). Counts: **0 BLOCKING / 0 MAJOR / 4 MINOR / 6 nits**.

VERDICT: CONVERGED
