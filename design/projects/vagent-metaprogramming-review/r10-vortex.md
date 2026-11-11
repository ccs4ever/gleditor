# Round 10 critique (vortex role): vagent-metaprogramming.md v1.9

Lane: VortexCore/VM execution semantics, store/spool positions, trail/marks,
handle relocation, promote, MeshActorRuntime invariants. Convergence check on
the v1.9 deltas: (a) the `E_ops` landmark split + mandatory canonical
comparand (:1801-1836, :1427-1434, §10.1 :2390-2412, P16 row :2443), (b) the
spawn-side `WorkerContext(base, reader)` base-injection (:1400-1417,
:1715-1721, :2428-2436), (c) the capture-endpoint rehydration restatement
(:1855-1864, :1873-1889).

## Verification of the r9 dispositions

**R9-A — verified fixed end-to-end.** The text now names the two landmarks
separately and every load-bearing citation checks out:

- `E_ops` = image prefix length *in ops*, the ref-space ordering bound
  (:1803, :1427-1429). A persistent `CellRef` is a birth-op position:
  `Store::cellRefOf` returns `opsSpool.indexOf` of the mint op
  (`store.hpp:677-681`), the fold sets `.birthOp = opIndex` verbatim
  (`manifold.cpp:434`), `birthOp == this CellRef` (`manifold.hpp:295`).
- "The image's last op is never a birth" — `promote` mints all links last
  ("Links last: both ends have to exist", `arena_manifold.cpp:2010-2017`,
  the link loop :2020-2053 after the mint loop :1863-1982 and cyclic-handle
  setValues :1984-2003); `exportSymbol` likewise closes with `+d.values` and
  the rank link (`vortex_core.cpp:119-127`). The walk-max assertion is gone
  ("no walk maximum can ever equal `E_ops` and none is asserted", :1807).
- Per-cell classification replaces refusal (:1820-1828): each `program()`-
  walk cell tests its own ref against `E_ops` — `≤` program-side, `>`
  state-side, classified not refused — so the `+d.code`/`d.vars`/`d.belief`
  reaches that killed v1.8's walk now classify correctly. The walk's root is
  named — "the adapter-recorded program root resolved on the received fold"
  (:1822) — closing r9's binding-order gap ((c) re-binds for in-arena use).
- Merged-lineage detection moved to a mandatory ops-segment content
  comparand (:1807-1820): canonical post-remap form, op kind + non-scroll
  fields — correct, since `applyOpsSegment` rewrites *only*
  `record.op.span.scroll` through the remap table
  (`publication.cpp:1048-1070`, `:1079-1086`; everything else "travels as it
  was written", :1075-1078). The permutation/scramble justification is
  accurate — `adoptOpRecords` topo-resorts ready siblings by `produces`
  hash (`store.cpp:3836-3940`, priority queue :3909-3913). `mergeValues`
  arms now split correctly: `(contentHash, authorKey, parent)` sort
  (`mesh_value.cpp:519-523`) into `base->store` under `job.base`
  (`mesh_host.cpp:507`) or a prefix-less fresh store otherwise (:509-510) —
  the fresh arm "fails the comparand outright", the `job.base` arm "appends
  above the base's own prefix". Both true.
- The lineage inheritance chain is intact: checkpoints append, whole-spool
  seal (`mesh_value.cpp:227-228`,`:241`; `publication.cpp:611-617`),
  `rebuildManifold` assigns `birthOp = opIndex` (`manifold.cpp:427-442`),
  `noCell == 0` the only ref below the band (`dim_vector.hpp:23`).
- Supporting fixes all landed: `historyFromSeal` named as the public apply
  seam with `applyOpsSegment` file-local (`publication.cpp:1093-1099`,
  anon-namespace :1023-1091, `\x7fXSO\x01` gate :1033, `sealedOpsName` =
  `"ops"`, `provenance.hpp:286`); `ops.nodes` distinguished correctly;
  `ArenaManifold::store()` named public (`arena_manifold.hpp:269-271`,
  falling to `base_->store()`); `Manifold::store()` non-const return
  accurately described (`manifold.hpp:486`); promote's fresh-store
  incoherence argued correctly off `real`'s identity map (:1867-1869),
  `OpHandle` target translation (:1947-1951, `:1995-1996`) and `setLink`
  emission (:2041-2042).

**R9-B — verified.** The spawn-side injection shape is stated correctly:
`WorkerContext(base, reader)` binds `arena(baseManifold)` *before*
`core(*arena.setDefaultSpanReader(reader))` runs `initGenesis`
(`worker_context.hpp:52-55` → `vortex_core.cpp:14`), which adopts
`*base->home()`/the base dims only when a base exists (`:30-43`); both are
private with no re-genesis door (`vortex_core.hpp:268`,`:282`,`:287`), so a
post-construction `attach(Space)` cannot re-deliver them — the doc's exact
argument (:1400-1406). `spawn()` takes no store (`mesh_actor.cpp:119-122`,
the bare `WorkerContext` mint `:156`); `resume` injects the folded
checkpoint manifold (:270-271) and keeps its `receiveValue`-built store in
`actor->checkpoint` (:260-269). `rebuildManifold` returns the `Manifold`
*by value* (`store.hpp:450-451`), so §10.1's "must pin for the arena's
life" is real, not decorative. The `documentId` choice is now stated with
real machinery: `documentId_` is `Store`-ctor state (`store.hpp:133`,
`:1435`) and `restorePublication` already adopts a sealed id
(`publication.cpp:1127-1128`). "Supply-side, not a store parameter"
reconciles the P16 row (:2430-2431, :2443).

**R9-C — verified, with two wording reservations (findings 3, 4).** The
capture-endpoint mechanism is coherent with §6.1's recorded-template rule:
recipe endpoints are `+d.values` links to cells, "never a census integer"
(:1095-1097); on the received fold a capture endpoint resolves to the fold
cell at the promoted position, so `Value`-replaying `int64(endpoint)`
writes what `*handleTarget` carries on the donor slot; `reify`'s
`New`/`Link`/`Value` palette cannot mint handle-kind cells, so the copy's
slots arrive `Int64` and the (e) kind-guard no-ops on them (:1878-1880);
`importValue`'s demote/local-twin remap vs `resume`'s verbatim fold is
described accurately (`mesh_value.cpp:485-501`; `mesh_actor.cpp:260-271`;
`arena_manifold.cpp:1947-1951`). The (d)→(e) install-set hand-off
(:1857-1862) and the post-installation-symbols ordering argument
(:1883-1887) are consistent.

## New findings (v1.9 deltas only)

### 1. MAJOR — genesis (b)'s "every op cell in the `1..E_ops` band" has no identification mechanism, and the obvious one poisons P15

**Location:** :1839-1841 (and the parallel :1312-1313, P15 row :2442).

**Problem.** The re-registration pass must insert *exactly* the image's op
cells — under-registration leaves op cells outside the P15 refusal (the
surgical-write hole the pass exists to close), over-registration makes P15
refuse *honest* writes. In the folded band, an op cell is an `Int64` cell —
indistinguishable from every other `Int64` cell by any stored field. The
implementation an engineer reaches for — a band scan keyed on
`getOpcodeKind`'s `asInt64 ≤ Linked` fallback (`vortex_vm.cpp:107-112`;
`Linked` is the 48th and last enumerator, `vortex_vm.hpp:27-93`) — registers
every band cell holding `int64 ≤ 47`. Such non-op cells exist in every
ordinary image: `dimConstant` operand cells hold `int64(dimBirthPos)`
(`compiler.cpp:769-772`) and genesis/system dims occupy the band's lowest
positions, so a folded ref cell naming `d.grab`/`d.vars`/a module cell
decodes as `Mul`/`Eq`/…. After misregistration, P15's
`opcodeMap_.contains(masterOf(cell))` refuses `Value`-writes to them — a
bound output or var cell caught in the set breaks the honest program
post-resume. The structural alternative under-registers instead: op cells
reached only through `int64` content (`Jump`/`Call`/`Branch`/`Choice`
targets, a separately emitted sequence with no inbound `+d.spin`) are
invisible to any outbound link walk — the document itself bounds the walk
exactly so (:1498-1500). The only complete discriminator is the recorded
instruction set — the same set `mesh_evaluator.cpp:658-663` already
registers from `program.instructions` — or an explicitly stated op-graph
rule (the §7.2 `validate` traversal vocabulary, `:2035`, is the closest
stated one but is per-fragment and never linked from (b)). The v1.9
sentence "every op cell in the `1..E_ops` band" presumes an oracle.

**Fix.** Name the source: "the op-cell set is the image's instruction
enumeration — recorded with `E_ops` on the `programRef` bind record (the
`program.instructions` pattern of `mesh_evaluator.cpp:658-663`), replayed
through `setOpcodeKind` on the folded refs. A decode scan is the one wrong
shape: `asInt64 ≤ Linked` aliases every small-valued ref cell
(`dimConstant` slots naming genesis-era dims), registering data cells into
P15's refusal."

### 2. MINOR — the comparand's reference operand is ambiguous between the raw segment and the adopted spool

**Location:** :1808-1810 — "the adapter holds the artifact's seeded
segment, and asserts the received store's ops `1..E_ops` decode to the
same op sequence."

**Problem.** `adoptOpRecords` re-orders simultaneously-ready records by
`produces` hash (`store.cpp:3909-3934`), so a *branched* segment's record
order ≠ the adopted spool order — on the honest path too, not only under
attack. A comparand read as received-spool vs *raw segment records* refuses
the honest branched-band lineage it exists to tolerate. The coherent
reading — both sides' post-adoption band decodes — is supported by the
surrounding text ("§10.1's seeding step owns the store", :1829-1831;
"the adapter's own `Store`", :1378-1380) but this sentence names "the
artifact's seeded segment", i.e. the adoption *input*. (Detection is
unaffected: adoption is deterministic, so a received band can only differ
in order if its records differ — caught by content either way. Only the
honest-path branched band is at risk, and single-parent image chains —
`promote`/sequential emission — are immune because their topo order is
forced.)

**Fix.** "…against the adapter's seeded *store's* ops `1..E_ops`, both
decoded canonically — never the raw segment records, whose order adoption
may itself permute (`store.cpp:3909-3934`)."

### 3. MINOR — "the identical bits `*handleTarget` on the donor slot names" holds only for capture endpoints

**Location:** :1874-1878.

**Problem.** For an *external* referent the claim is exact: the recipe's
capture link resolves on the received fold to the promoted position, and
`int64(endpoint)` writes the same bits `*handleTarget` carries on the
donor's checkpointed slot. For an *in-fragment* referent the mechanism is
§6.1's old→new remap (:1098-1099): the copy's slot deliberately names the
*copy's own twin* — different bits by design. The conclusion (slots born
`Int64`, kind-guard no-op, pre-rehydrated) stands for both, but the stated
identity covers only the capture arm.

**Fix.** "— for capture endpoints `int64(endpoint)` writes the identical
bits `*handleTarget` names on the donor slot, while in-fragment referents
derive through the old→new map to the copy's own twins; either way the
copy's slots arrive `Int64`…".

### 4. MINOR — "each record's `+d.values` names the referent" points at a field the `+d.refs` schema does not show

**Location:** :1875 (with the ref-record schema :1008-1017).

**Problem.** The `+d.refs` record schema gives a ref-record `+d.ref`→slot,
`+d.name` (name-records), `+d.grab`→role — no `+d.values`→referent. The
`+d.values`-as-endpoint convention belongs to the *recipe's* link-tuple and
capture records (:1095-1097, :1018-1019), which is where the derivation
actually lives (per finding 3: capture link or old→new remap, both recipe
machinery). If the referent edge is meant to sit on the ref-record itself,
the fragment-head layout must grow the field; if it lives on the recipe's
capture records — sufficient, since registered heads rehydrate from
`*handleTarget` and copies from the recipe — the sentence is attributing
the mechanism to the wrong record type and an implementer reading the
diagram builds records missing the edge the genesis story names.

**Fix.** Reword to "the regenerated records' *recipe* capture endpoints —
each capture/link-tuple record's `+d.values` naming the referent cell" —
or add `+d.values`→referent to the `+d.refs` ref-record schema at
:1008-1017 explicitly.

## Items verified clean in the v1.9 deltas

- **Sideness/classification plumbing** — `ephemeralBit` refs never satisfy
  the persistent arm (`manifold.hpp:154-158`; `refOf`,
  `arena_manifold.hpp:468-470`); shadows name their base ref
  (`arena_manifold.cpp:1489-1490`) so a pre-extent-base shadow classifies
  program-side "by design"; `promote`'s BFS prunes at non-`holdsOwn`
  persistent cells (`arena_manifold.cpp:1755-1757`); `d.dims` skipped
  (:1766); `ensureDimension` = base → `arenaDims_` → mint (:78-105).
- **Marker/record emission** — per-ref marker mint precedent
  `mesh_evaluator.cpp:735`; one `-d` neighbour per dim, eviction
  `arena_manifold.cpp:1347-1377`; `isCellRelocationHandle` requires
  OpHandle kind + marker edge (`foreign_op_handle.cpp:98-117`);
  `OpHandle`/`ForeignOpHandle` kinds `ops.hpp:154-163`.
- **Ceremony cites** — `pending`/`checkpointView`/`pumpOne`
  (`mesh_actor.cpp:659-662`,`:672`,`:462`,`:467-468`,`:473-477`); the
  Executor signature exposes `WorkerContext&`, no store
  (`mesh_actor.hpp:113-116`); `ActorCheckpointView` is
  `const ArenaManifold&` (:102-106); `publishValue` takes any `Store&`
  (`mesh_value.hpp:69-74`); `setCursorOpcode`/`Call` frames/`Halt`
  semantics (`vortex_vm.cpp:139-143`,`:854-893`,`:1220-1225`).
- **`importValue` vs `resume` contrast** — verbatim.
- **`plan_named`/handler/`code_named(L)` minor batch** — clause `+d.name`
  verbatim matching and the anonymous-handler gap stated consistently
  (:1556-1582); headless-`run` refusal tied to the descriptor lookup
  (:1154-1158).

## Nits (non-blocking)

- :1717, :2430-2431 — the `WorkerContext(base, reader)` cite lands on a
  call that passes *one* argument (`mesh_actor.cpp:271`); the reader is
  set post-construction at `:273`. The two-arg form is the header
  signature and is strictly better for the proposed spawn shape — worth
  half a clause so the cite does not read as if the call already uses it.
- :2411 — "the recorded program-image extent field genesis (a) asserts":
  post-split, (a) asserts the band *comparand*; the extent is its bound.
  "the `E_ops` field genesis (a)'s comparand consumes" tracks v1.9.

## Summary for the orchestrator

R9-A's landmark split is correctly implemented end-to-end: `E_ops` is the
ops-length ordering bound, walk-max is gone, per-cell classification
replaces refusal, and the mandatory canonical comparand now carries
merged-lineage/permutation detection — every citation verified.
R9-B's base-injection and R9-C's capture-endpoint restatement are sound.
One new MAJOR: genesis (b) says "every op cell in the `1..E_ops` band"
re-registers into `opcodeMap_` but never says how op cells are identified —
the natural `asInt64 ≤ Linked` scan registers small-valued ref cells
(`dimConstant`s naming genesis dims at positions ≤47) and P15 then refuses
their honest writes; the fix is the recorded instruction enumeration
(`mesh_evaluator.cpp:658-663` precedent) or a stated op-graph rule.
Counts: **0 BLOCKING / 1 MAJOR / 3 MINOR / 2 nits**.

VERDICT: NOT CONVERGED
