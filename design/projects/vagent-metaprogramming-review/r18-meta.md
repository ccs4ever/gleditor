# Round 18 critique — `design/vagent-metaprogramming.md` v1.17 — Metaprogramming Expert lane

Scope: convergence-freeze verification. (1) Did the r17 items land *correctly* — the positional
`+d.step` carrier, the `+d.name` dim-discriminator reservation, the `+d.refs` scoping, and the
filing ordering — and does the positional carrier preserve the recipe's self-describing property
under a generic reify walk? (2) Is the staging model still closed at the *mechanism* level — every
admitted endpoint class durable? Full v1.16→v1.17 diff reviewed (`git diff 6ee49636..909062ac`);
code citations re-verified against `apps/common/xanadu/`.

## 1. R17-A landing — positional `+d.step` carrier + `+d.name` reservation: **LANDED, one clause owed**

The pin is applied everywhere the schema carrier appears: head diagram (:1078-1080 — "four
`+d.step`-consecutive members in `(from, dim, dir, to)` order, the member's chain position its
field identity"), the §6.1 recipe paragraph (:1186-1189), the step-1 restatement (:2131-2133),
and the P16 row (:3070). The reservation is applied at every `+d.name`-on-`+d.recipe` site:
:1085-1087, :1119-1121, :1195-1197, the `closed` glossary row (:146) and arm (c) (:1945-1947),
(e)'s dim arm (:2369-2375), and the P16 row. The discriminator scoping — "`+d.name`'s presence
**on a `+d.refs` record**" — is correct at :1119, :2030-2031 and P16; the only unscoped
"`+d.name` present marking a name-record" left standing is inside the 1.3 changelog entry (:3386),
historical record under the doc's confess-don't-rewrite convention.

**Does a generic reify walk reconstruct field identity without a name channel? Yes — but only
through a `dim`-anchored block inference the document never states.** The recipe is one flat
`+d.step` chain holding four record classes — link-tuple fields, literal records, capture records,
ref-capture records (:1078-1100, :1971, :1980; the composition is unchanged since the recipe was
introduced). Raw chain index cannot be field identity: literal and capture record *subjects* are
marked `int64` ref cells exactly like `from`/`dir`/`to` members ("record subjects alike", :3894),
no per-record class signal is stated for them, and the recipe carries no census — so under an
interleaved or even unannounced layout, member *i* mod 4 is not a field. What rescues the decode
entirely within the stated vocabulary is that **every tuple carries exactly one `dim` field at
fixed index 1, and `dim` members are the only chain members bearing `+d.name` without
`+d.refFor`** — the reservation makes `+d.name` unambiguous (dim-name channel only), and v1.17's
own "a `+d.recipe` field's ref-record pairs with nothing — no `+d.refFor`" (:1206-1208,
:2367-2369) splits it from the ref-capture dim record. The walk: each `+d.name ∧ ¬+d.refFor`
member is a `dim`; its predecessor is `from`, its two successors `dir`, `to` — valid precisely
because "four `+d.step`-consecutive members" pins within-tuple adjacency — leftovers classify by
`+d.refFor` (ref-capture) and shape (literal/capture). N is likewise derivable as the `dim`-member
count for the contiguous-layout reading of the diagram order. So the self-describing property
**survives** — field identity is total and corruption-detecting — but "the member's `+d.step`
position" read literally is only true under a layout the text does not pin, and a generic reader
implementing raw-index decode corrupts under interleaving. Finding 1 asks for the anchor clause.

Everything else about the landing is sound: the carrier is pure structure, so it folds verbatim
(`+d.step` links translate like every link — no re-mint ceremony needed for `from`/`dir`/`to`,
unlike the `+d.values`-scalar-cell alternative the disposition also offered); the copy's
regenerated recipe re-emits under the same rule (:1262-1263); and the reservation's side effect
— a capture record can never carry a variable name on `+d.name` — is safe because replay finds
the binding through the donor slot's `-d.clone` backlink, not a name.

## 2. R17-B landing — filing coverage and ordering: **CORRECT, verified against code**

The ordering is pinned on both lineages and the mechanism cited checks out:

- **Spawn-seed**: files the whole vocabulary on the kept lineage `Store`'s persistent `d.dims`
  rank *before* the folded base is pinned — "`getOrCreate` move-assigns its `Manifold&` argument
  (`dimension_registry.cpp:194-217`), so the rebuilt fold the filing returns IS the
  base-injected fold" (:2994-3000, echoed :118-120, :552-556, :2280-2287, :3260). Verified:
  `getOrCreate` ends `manifold = std::move(newManifold)` (:214) after `makeDimension` +
  `rebuildManifold` + `setStore` — the pinned base genuinely is the post-filing fold, so
  `dimensionNamed`'s base arm (`manifold.cpp:782-789`, `held`) sees the filed cells and no
  ephemeral twin is ever minted. The r17 file-after-pin trap is closed.
- **Emit**: "files the same vocabulary the same way before ITS lineage's first resolution —
  before the compile arena's `initGenesis` (`vortex_core.cpp:14,:29-70`)" (:2283-2285,
  :2999-3008). Verified: `initGenesis` runs in the `VortexCore` ctor (:14) and each
  `mintNamedDimension` calls `baseDimensionNamed` first (:16-27) — filing first makes the
  genesis bind, not mint, so `int64(DimRef)` operands and dim-keyed edges seal naming persistent
  cells.
- **Coverage**: the enumerated set is now the whole minted vocabulary — bind set + `attach()`'s
  `d.store-refs` (`arena_manifold.cpp:289`, verified `ensureDimension("d.store-refs")`; `d.stores`
  at :288 is already a V10 `mintNamedDimension` name, `vortex_core.cpp:68`) + `initGenesis`'s
  V10 names (`vortex_core.cpp:50-69`, verified verbatim) + `dimConstant`/`findOrMintDimension`'s
  mints (`compiler.cpp:769-772`, verified) — defined as "the `d.dims`-rank member set
  `dimensions()` walks (`manifold.cpp:771-775`)" — verified `rankAfter(*this, *home_, *dimsDim_)`
  at :774-775, and every mint path does append there (`ensureDimension`'s tail-append :90-99,
  `mintNamedDimension`'s link :21-23). The under-covering "every `ensureDimension`-minted name"
  is gone.
- **Honest residual**: "a name first minted mid-session stays outside the filed set — its
  orphan accretion" (:3007-3009, :2376-2380) — correct and bounded (one orphan per unfiled name
  per checkpoint, name arm self-heals). The only names this can still catch are machinery mints
  outside the fragment vocabulary — `d.op-label` (`mesh_value.cpp:473`, inside `importValue`,
  which `resume` skips anyway) and `OrParallelCoordinator`'s
  `d.or`/`d.branch`/`d.frontier`/`d.submission` (`or_parallel.cpp:181-184`), all mid-session and
  never named by a recipe field — nit-listed below for completeness.

## 3. Remaining r17 minors — all verified landed

`closed` arm (c) gained the `dim`-field member form (:1945-1947, matching glossary :146 and
re-verify :1979-1981); `reify` re-mints every `+d.name` name cell verbatim including the
`dim`-member's dim-name cell, with the single-occupancy reason stated (:1257-1261); (e)'s
"re-minted canonical cell" corrected to re-bound-off-filed-rank / re-mint-only-unfiled
(:2376-2380); (d)'s `+d.name` exception mirrored for the member (:2349-2350); `+d.clone`
direction scoped to first capture in §6.6 and all three changelog echoes (:2253, :3819,
:3839, :3886); `set_weight`'s third case stated (:2630-2632 — see finding 2); the
:836/`cloneMaster` cite split (:843-845); "the member cell itself" phrasing (:2375); the §8.3
miscite re-pointed to the synapse bullet + glossary (:2625); "(§6.1's uniform position
keyspace)" → "under the one-rank inventory" (:2134); "`DimRef` counting" as an int64 ref under
the uniform rule (:2120-2122 — consistent with the member/slot REF-record rule); ref-capture
records scoped to *slot* ref-records throughout (:1090-1093, :1206-1208, :2367-2369); the
filed-set enumeration raised beside the bind list it under-covered (:2980-2994).

## 4. Mechanism closure — still closed at the mechanism level; no new MAJOR

Re-walked every admitted class under v1.17: external/internal int64 endpoints (mark → `OpHandle`
→ `*handleTarget` → (e) restore), slot dim referents (`+d.refFor` record `+d.name` →
`ensureDimension` both at replay and (e)), tuple `dim` fields (member `+d.name` → same arm, write
into the member cell), `cap$N` captures (wing-dim links, `masterOf`-decoded, re-captured
`New`+`Link` at the `d.clone` tail — first vs chained direction now correct), call/run bindings
(role-split name-records), and the one-inventory `+d.refs` sweep covering slots and recipe
fields alike. The positional carrier adds no durability surface — `+d.step` links fold verbatim
like all links, so the recipe stays self-describing across checkpoint for (d)'s reify of
registered heads. The only reachability boundary (installed copies on install-site anchors) is
still covered by (d)→(e)'s install-set hand-off. R17's closure verdict stands.

## 5. Findings

### 1. MINOR — §6.1/:1078-1080 + :1186-1189 + :2131-2133 — the positional carrier's *decode* is unstated: "chain position" is not field identity on a heterogeneous chain

The pin says "the member's `+d.step` chain position its field identity" but the recipe chain
carries literal records, capture records and ref-capture records beside the tuple fields, all on
the same `+d.step` links — and literal/capture subjects are marked `int64` members shape-
identical to `from`/`dir`/`to` (marked, `+d.ref`-paired, `+d.values`-optional), with no census
and no stated class signal. Raw-index decode is therefore unsound under any non-segregated
emission order, which the text nowhere forbids or requires. The model *is* still self-describing
— the working decode is fully derivable: `+d.name`-present, `+d.refFor`-absent ⇒ the `dim`
member (ref-capture dim records split on `+d.refFor`; the reservation bars `+d.name` on every
other class), each tuple's `dim` sits at fixed index 1 of a stated-consecutive 4-block, so its
predecessor is `from` and successors `dir`, `to`, and N is the `dim`-member count under a
tuples-first layout — but no sentence says so, and a generic reify walk implementing the literal
claim mis-keys under interleaving, recreating exactly the wrong-arm class R17-A existed to kill.
**Fix (one clause, either arm):** "the walk groups link-tuple members `dim`-anchored — the
`+d.name`-bearing, `+d.refFor`-free member is `dim`, its predecessor `from`, successors `dir`,
`to`; every other record class follows or is identified by `+d.refFor`/subject shape" — or the
layout arm — "link tuples occupy the recipe's first 4N `+d.step` members, N read as the count of
`+d.name`-bearing non-`+d.refFor` members; literal, capture and ref-capture records follow".

### 2. MINOR — §8.1/:2624-2636 — the `set_weight`/`weight_of` dispatch still isn't total: the adopted third case leaves a fourth

The dispatch now enumerates {junction (`+d.weight`-bearing `+d.synapse` member) → own scalar;
clause → `+d.name` → qualified key; `+d.name`-bearing non-junction non-clause (a head label) →
label-verbatim key; `+d.name`-less *clause* → refused}. A `+d.name`-less target that is neither
junction nor clause — a plain state cell, a `d.vars` binding cell, an op cell — is a legal
focus-slot argument and hits no arm at all, where "only named clauses key" says it must refuse
like the name-less clause. **Fix:** widen the last arm — "a `+d.name`-less non-junction target,
clause or not, resolves no key and is refused like any unresolved focus-slot verb".

### Nits

- :3003-3009 — "every name the lineage's arenas can resolve" over-reads slightly beside the
  enumerated set: machinery mints (`d.op-label` `mesh_value.cpp:473`; `d.or`/`d.branch`/
  `d.frontier`/`d.submission` `or_parallel.cpp:181-184`) are resolvable yet outside it. All are
  mid-session mints no recipe field names, squarely inside the stated honest-residual clause — a
  parenthetical ("machinery mints outside the fragment vocabulary ride the same residual") would
  preempt the question.
- Pre-existing, surfaced for the record only: literal-record and capture-record member *shapes*
  are still unstated anywhere (which edge, if any, marks them). Harmless to the tuple decode —
  the `dim`-anchor resolves tuple-vs-rest without them — but finding 1's fix will want to name
  their identifying signals for completeness.

VERDICT: CONVERGED
