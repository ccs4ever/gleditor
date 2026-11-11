# R10 — ZigZag topology/promotion critique (v1.9)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.9 deltas in my lane: genesis (a)
two-landmark rewrite — `E_ops` + mandatory ops-segment comparand + per-cell classification
(:1801-1836), (b)'s registration scope keyed on the comparand-verified band at spawn *and* resume
(:1837-1850), (d)'s capture-record endpoint derivation (:1853-1862), (e)'s coverage/ordering
relabel (:1863-1889), the §6.6 lineage paragraph edits — spawn-side base-injection, `store()`
verify arm, `documentId` handling (:1376-1471), the `home` prune-weight edit (:1486-1495), and the
minor batch (`1..opsTotal` band :620, `patch:<n>` ordinal :1555-1560, `learn_handler` additive
:1574-1582, `code_named(L)` unresolved-at-emission :1621-1633, `+o:` arm unit :1920-1923, §5.6
*no program lineage* diagnostic :969-971).

Verified against `arena_manifold.cpp` (`promote` BFS :1744-1788 — prune at any non-ephemeral
non-`holdsOwn` cell :1755-1757, `d.dims` skip :1766, both-direction discover :1781-1783,
relocation-handle enqueue :1759-1764, `OpHandle` gate :1797-1809, `real` identity-map for
non-ephemeral cells :1863-1869, `makeOpHandle(real.at(target))` :1947-1951 and the delayed
`setValue` arm :1992-1996, link emission held-sources-only with translated endpoints :2010-2042;
`asInt64`/`handleTarget` kind gates :1082-1097; `link` displacement :1345-1377), `manifold.cpp`
(`birthOp = opIndex`, `byRef.emplace` :427-442), `manifold.hpp:295` (`birthOp == this CellRef`),
`store.hpp` (`cellRefOf` → `opsSpool.indexOf` :677-681; `documentId_` ctor state :133,:1435;
`rebuildManifold` by value :450-457), `store.cpp:3830-3940` (`adoptOpRecords` produces-ordered
Kahn), `publication.cpp` (scroll remap :1048-1086 feeds `adoptOpRecords` :1088;
`historyFromSeal` :1093-1099; `\x7fXSO\x01` gate :1033; `Store(readerPermascroll, pub.storeId)`
:1127-1128), `mesh_value.cpp` (`(contentHash, authorKey, parent)` sort :519-523),
`mesh_host.cpp:506-510` (`job.base`→`base->store` vs fresh `empty`), `mesh_jobs.cpp:304-318`,
`mesh_actor.cpp` (`WorkerContext()` no-base mint :156; spawn signature :119-122;
`WorkerContext(checkpoint->manifold.get())` :270-271; `programRef` pin :199,:215-216),
`worker_context.hpp:52-55`, `vortex_core.cpp` (`initGenesis` in ctor :14, base home/dims adopt
:29-70; `exportSymbol` closes with `+d.values` then rank link :118-127), `vortex_core.hpp:282,287`
(`initGenesis`/`home_` private), `vortex_vm.cpp:96-114` (`setOpcodeKind`, `getOpcodeKind`
map+`asInt64` fallback), `foreign_op_handle.cpp:98-117`, `mesh_evaluator.cpp:731-735`,
`arena_manifold.hpp:269-271` (`store()` public), `arena_manifold.hpp:468-470`
(`refOf = ephemeralBit|dense`), `compiler.cpp:769-772` (`dimConstant`), `provenance.hpp:286`
(`sealedOpsName`), `mesh_actor.hpp:102-106` (checkpointView: continuation/arena/root, no store).

## Verdicts on the r9 majors' fixes

### Fix 1 — un-pruned walk / positional assertion → two landmarks: **resolved in substance**

The new (a) is mechanically consistent:

- **Landmark separation is accurate.** `E_ops` = image prefix length *in ops*; a cell's persistent
  ref is its birth-op position (`manifold.hpp:295` — `birthOp` "== this CellRef",
  `store.hpp:677-681`). The image's last op is never a birth — `promote` emits links last
  (`arena_manifold.cpp:2010-2017`), `exportSymbol` likewise closes with the `+d.values` and rank
  links (`vortex_core.cpp:118-127`) — so no walk maximum can equal `E_ops`, and none is asserted.
  Since `E_ops` is a non-birth position, `ref ≤ E_ops` and `ref < E_ops` coincide; the bound is
  tight.
- **The comparand is the right form and the right detection.** "Canonical post-remap form, op kind
  plus non-scroll fields" — `applyOpsSegment` rewrites `record.op.span.scroll` through the
  segment's table before `adoptOpRecords` (`publication.cpp:1048-1088`), so a byte compare would
  indeed refuse every honest resume. Within-band permutation is a reachable shape exactly as
  claimed: `adoptOpRecords` is a `produces`-min Kahn sort (`store.cpp:3830-3940`); a linear
  `parent` chain admits a unique topo order, so only *branched* record sets permute — which is
  what the sentence says. The same deterministic sort runs on both sides (seed via
  `historyFromSeal`, resume via the checkpoint's segment), so honest lineages reproduce the same
  band — no false refusal.
- **Per-cell edge cases close.** *A state cell inside `1..E_ops`*: impossible under a passed
  comparand — a cell's ref is its birth op's index, and every op at `1..E_ops` is a comparand-
  verified image op, so every band-born cell is an image cell. The text's own answer is correct:
  "a non-image cell inside `1..E_ops` is what the comparand just excluded" (:1827-1828). *A program
  twin minted above `E_ops`*: classifies state-side — correct, since a twin is imported state for
  this lineage (the `job.base` merge arm lands twins above the base prefix, `mesh_host.cpp:507`,
  and the no-base arm fails the comparand outright, `:509-510` — both verified). Ephemeral cells
  take the non-persistent arm per §6.6's sideness paragraph (:1438-1441).
- **Fail-closed coverage is now honest**: permutation/scramble → comparand; non-image ingress into
  the program-side set → position classification; prefix-less lineage → comparand fails → genesis
  bind refusal plus the in-session `learn` refusal on the empty program-side set (:1833-1835,
  §5.6 diagnostic :969-971).

The residual defects are in Findings 2 and 3 (MINOR).

### Fix 2 — copy pre-rehydration → capture-record endpoints: **direction verified sound under `promote`'s translation; the carrier edge is missing — Finding 1**

The derivation is mechanically sound *if* the endpoint edge exists: the referent reaches the fold
by two redundant paths — the marked slot's `OpHandle` enqueue (`arena_manifold.cpp:1759-1764`,
emitted `real.at(target)` :1947-1951) and the record's `+d.values → referent` link edge, which
`promote` emits from the held record with `real.at(edge.pos)` (:2020-2042) — landing on the same
fold cell at the same promoted position. In the received manifold, `int64(the record's +d.values
neighbour)` is therefore bit-identical to `*handleTarget` on the donor slot — for **external**
referents, exactly as :1875-1877 claims. The rule also reconciles §6.1's recorded-template rule
(endpoints-as-cells replayed verbatim is what "never a census integer" was designed for), and
(e)'s ref-arm kind-guard correctly no-ops the pre-rehydrated Int64 copy slots (:1787-1789,
:1878-1880). But the edge itself is contradicted by the stated schema, and the derivation is
mis-scoped twice — MAJOR, Finding 1.

### Other verifications

- `home` prune-weight edit (:1486-1495) — my r9 nit adopted verbatim: "no *system-minted* edge"
  + "the prune is what excludes the frames in every case" with the correct `holdsOwn` cite
  (:1755-1757). Resolved.
- (b) widened to spawn — consistent: `opcodeMap_` is per-`WorkerContext` and empty at both binds;
  `setOpcodeKind` pass over folded cells is the right shape (vs `mintOpcode`, which mints
  ephemeral twins).
- Spawn-side base-injection — verified end-to-end: `attach(Space)` installs `base_`/`store_` only
  when unset (`arena_manifold.cpp:274-287`) and cannot re-run `initGenesis` (private,
  `vortex_core.hpp:282`; adopts `*base->home()` only when a base exists, `vortex_core.cpp:30-36`);
  `WorkerContext()` at `mesh_actor.cpp:156` is baseless today, so the seam is genuinely owed;
  `WorkerContext(base, reader)` is a valid ctor shape (`worker_context.hpp:52-55`) — resume
  passes only the base and sets the reader after (`mesh_actor.cpp:270-273`), trivial imprecision.
- `documentId` handling — `documentId_` is `Store`-ctor state (`store.hpp:133,:1435`), the segment
  apply carries ops never identity, `Store(readerPermascroll, pub.storeId)` precedent
  (`publication.cpp:1127-1128`). Accurate.
- `1..opsTotal` sealed band, `ops` segment name/`historyFromSeal` seam, `mergeValues` order and
  `job.base` scoping, `patch:<n>` ordinal + user-name refusal, `learn_handler` additive +
  anonymous-handler gap, `plan_named`/`label_of`/`code_named(L)` minor-batch items — all read
  consistently with the verified code.

## New findings (v1.9 deltas only)

### 1. MAJOR — the `+d.values → referent` endpoint edge is asserted, contradicted by the stated schema, and mis-scoped on two axes

(d) and (e) now hang the whole copy-rehydration mechanism on a graph edge that the design never
mints: "the copy's ref slots derive from the *capture-record endpoints* at replay" (:1855-1857);
"§6.1's recipe encodes endpoints as cells (each record's `+d.values` names the referent, never a
census integer)" (:1874-1875); changelog repeats it (:2977-2979).

- **The schema contradicts it — twice.** The unified `+d.refs` record schema enumerates record
  edges exhaustively: `+d.ref → the slot cell`, `+d.name` (optional), `+d.grab` role label — and
  explicitly rejects `+d.values`: "`+d.ref`, never `+d.values`: a later `+d.values` write could
  evict the marker — link eviction" (:1009-1011). The P16 step repeats it: "the mark is `+d.ref`
  on the record plus `d.mesh.relocation` on the cell — **not `+d.values`**, because `+d.values`
  has occupancy semantics — a later descriptor write could evict a `+d.values` marker"
  (:1740-1742). §6.1's recipe encodes `+d.values` endpoints only for *link-tuple* records
  (:1095-1097); under the stated schema a ref-box's referent is exactly a recorded census-int
  literal — the thing the derivation exists to avoid. An implementer building the schema as
  specified mints records with no referent edge; the replay derivation has nothing to read and
  falls back to the stale literal — reintroducing the exact bug R9-C ruled against.
- **"Each record's" overgeneralizes to name-records, where the edge is forbidden.** If a
  `+d.name`-carrying record also carried `+d.values → its resolved target`, a program-side target
  (every `call`-role record's callee, `exportSymbol`'s entry op) would put a `d.code`-rank-
  resident edge into the program skeleton — precisely the invariant :1348-1350 states ("no cell
  on the agent's `d.code` rank may carry an edge in either direction into a program-skeleton
  cell"). It would also freeze the emission-time target, defeating the lazy rebind the
  name-record exists for (:1700-1705).
- **"Identical bits to `*handleTarget`" holds only for external referents — internal ref-boxes are
  the majority class.** The uniform marking rule covers *every* `makeScalarCell(int64(ref))` —
  wing slots, `reference()` cells (Jump/Branch/Choice targets — fragment-internal ops,
  `compiler.cpp:516-518`), dim-constants (:1730-1735). For an internal referent the donor's
  `*handleTarget` names the *donor's* internal position, while the copy needs `int64` of its own
  ephemeral twin through the old→new map (`:1098-1099` — the map already owns this case).
  `Value`-replaying `int64(endpoint)` with the donor's endpoint would bind the copy's control
  flow back into the registered head's graph — an aliasing misbind worse than staleness.
- Proposed fix — one schema sentence plus one replay rule, both mechanical: **each `+d.name`-
  absent ref-record carries `+d.values → the referent` for *external* referents** (state-side by
  `closed`'s arm (a), so the edge never violates the `d.code` invariant on registered heads);
  internal referents stay on the old→new map — or equivalently state that record endpoints remap
  through it like tuple endpoints, so the copy's record names its own twin uniformly; name-records
  never carry the edge; and either pick a dedicated dim (matching `+d.ref`'s two-named-ends
  convention) or keep `+d.values` with a clause stating nothing else writes it on records — the
  schema's own occupancy caution (:1009-1011, :1740-1742) must be reconciled either way. Side
  benefit worth stating: the edge brings external referents onto the `closed` walk's enumerable
  frontier (:1612-1616), so a smuggled program-side referent becomes link-visible.

### 2. MINOR — the named `>E_ops` legs are vacuous on the received fold

":1824-1827 cells the walk legitimately reaches beyond the band through `+d.code`, `d.vars` or
`d.belief` (registered heads, member state) are classified state-side, never refused." On the
received fold the outbound `program()` walk cannot reach any of those cells: the agent cell — the
spec's `stateRoot`, the one anchor of every agent state rank (`vagent-agent-language.md:126,:79`)
— is unreachable from `program()`, because a persistent program→agent `+d` edge can never exist:
`promote` persists link edges only from held (ephemeral) sources (`arena_manifold.cpp:2020-2042`),
and post-hoc arena edges from persistent program cells are never emitted. The cells the walk does
reach beyond the skeleton in-session — installed copies on `d.clause`/attachment anchors, `clause
+d.code` memo records, learned symbols on module `d.vars` — all die behind pruned program anchors
at promote (:1476-1479; `:1755-1757`), so they are absent from the fold too; and in-session they
are ephemeral, classifying state-side under the non-persistent arm (:1438-1441), not the `>E_ops`
arm the sentence describes. Net: the `>E_ops → state-side` arm is nearly dead code at (a) — which
is *harmless* (classification is not refusal, so correctness holds trivially), but the sentence
asserts reaches that do not happen and names cells ("registered heads, member state") that live
exactly on the unreachable agent ranks. Fix: reword to cover whatever the walk reaches (e.g.,
"installed copies, memo records and learned symbols reached through program anchors classify
state-side; agent-rank state is not `program()`-reachable at all") — or pin `program()`'s root
relative to `home` and re-derive the claim.

### 3. MINOR — "detected as misclassification" overstates; the per-cell test omits the ephemeral arm

":1826-1828 non-image ingress is detected exactly as misclassification: an image twin minted
above `E_ops` reads as state." A `>E_ops` twin is neither detected nor refused — it is simply
*admitted* as state by position (correctly — the `job.base` arm makes that admission legitimate).
The only detection that fires is the comparand. And "tests its own persistent ref against
`E_ops`" silently leaves ephemeral cells to §6.6's separate arm — one clause covering both
("persistent `≤E_ops` → program-side; `>E_ops` or non-persistent → state-side") would make the
test self-contained.

## Verified accurate (v1.9)

All new citations I could resolve check out: `arena_manifold.cpp:2010-2017` (links-last),
`store.cpp:3836-3940` (produces-ordered topo-resort), `mesh_value.cpp:519-523` (merge order),
`mesh_host.cpp:507` vs `:509-510` (base vs fresh arm), `publication.cpp:1048-1086`/`:1030+`
(remap feeds adopt), `:1093-1099` (`historyFromSeal`), `:1033` (magic gate),
`arena_manifold.hpp:269-271` (`store()` public), `worker_context.hpp:52-55` +
`mesh_actor.cpp:270-271` (base-injection shape), `vortex_core.cpp:14`,`:29-70` (genesis freeze),
`store.hpp:133`,`:1435` (`documentId_`), `:450-457` (`rebuildManifold` by value),
`mesh_actor.cpp:119-122`,`:156`,`:260-269` (spawn/resume store privacy), `mesh_actor.hpp:102-106`
(no store in checkpointView), `arena_manifold.cpp:1864-1867` (identity map), `:1947-1951`
(`real.at(target)`), `:2035` (`setLink`), `:1755-1757` (prune), `manifold.cpp:434` +
`manifold.hpp:295` (birth-op = ref = spool position), `vortex_vm.cpp:98-114` (`setOpcodeKind`/
decode fallback), `vortex_core.cpp:119-127` (exportSymbol closes with links), `compiler.cpp:769-772`
(dimConstant ref cells), `mesh_evaluator.cpp:731-735` (per-ref marker precedent),
`foreign_op_handle.cpp:98-117` (kind and marker), `store.hpp:677-681`, `provenance.hpp:286`,
`mesh_jobs.cpp:304-318`, `mesh_actor.cpp:199,215-216` (programRef pin), `vortex_core.hpp:282,287`
(`initGenesis`/`home_` private), `arena_manifold.cpp:274-287` (`attach` when-unset).

VERDICT: NOT CONVERGED
