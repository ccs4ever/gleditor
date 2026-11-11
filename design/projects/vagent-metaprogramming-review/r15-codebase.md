# r15-codebase: Round-15 codebase verification of `design/vagent-metaprogramming.md` v1.14

**Orchestrator summary (≤200 words):** All adopted r14 items landed correctly — the
`documentId_` nit is `Store`-ctor-or-load state at all three sites (:634-637, :1562-1564,
:2787-2789), the closed arm (a)/(b)/(c) restatement, the `*handleTarget` (d)-time read, the
recipe-tuple `+d.values` conditional, the declared-victim bound, the grafted-twin replay
caveat, the "no session resolver" mirror, the `store.cpp:123` cite fix — every one checks
out, and every assigned v1.14 citation verifies exact against the code. **But the R14-A fix
itself is broken a third way:** the restated agent-legs sentence (:2138-2139) names "a §5.3
`cap$N` capture slot resolving to its binding cell through `masterOf`/`d.clone`" as the
surviving cross-root edge into the `program()` walk — and no such traversal exists. The
slot→binding hop is `-d.clone` (negward) on a dim the walk's vocabulary does not list, and
`masterOf` is an operand decode, not a `+d` edge. The legs are simply *unreachable* — the
conclusion survives, stronger. **1 MAJOR, 0 MINOR, 1 NIT. VERDICT: NOT CONVERGED.**

## Citation verification — v1.14 delta cites (all pass)

- `store.cpp:123` — `Store::Store()` — the no-id signature (:123-125). Exact at :2861,
  :3612.
- `store.cpp:124` — `userPermascroll_(std::make_shared<UserPermascroll>())` — the fresh-mint
  arm's initializer. Exact at :641.
- `store.cpp:135` — `documentId_ = documentId;` in the two-arg ctor (:132-136). Exact at
  :636, :1563, :2788-2789.
- `store.cpp:4450` — `documentId_ = tables.documentId;` on `readStoreTables` directory
  load. Exact at :636, :1563, :2789.
- `store.hpp:133` — `Store(userPermascroll, const DocumentId &)` ctor param; `:1435` —
  `DocumentId documentId_;` member. Exact.
- `publication.cpp:1097` — `historyFromSeal` mints `std::make_unique<Store>()` — no
  permascroll/`DocumentId` parameter (:1093-1103). Exact at :639, :1565, :2790, :2856,
  :2861.
- `vortex_core.cpp:356-365` — `wingOf`'s operand lambda: `asInt64(slot)` → `CellRef` →
  `arena_.contains` guard → `.value_or(masterOf(slot))` (:358-365). The doc's "asInt64 fails
  → `masterOf(slot)`" (:823) is an exact gloss.
- `vortex_vm.cpp:807-836` — `Clone` walks `linked(cloneTail, dims().clone, false)` (false =
  POS, `fromNegward`, `dim_vector.hpp:85-86`) and `link`s the clone at the chain tail — the
  master's `+d.clone` posward rank (:813-822). "Clone → `d.clone` rank" exact (:1184-1185).
- `vortex_core.cpp:181`,`:202` — `masterOf(cell)` resolve at the read path (:181) and the
  `writeValue` funnel (:202). Exact at :334-336, :1185.
- `masterOf` itself — `arena_.cloneMaster(cell, dims_.clone)` (`vortex_core.cpp:344-346`) =
  `rankEnd(..., DimVector::NEG)` (`arena_manifold.cpp:1009-1012`) — walks **negward** to the
  rank head, the master (`zzcore.hpp:63-67`). Direction verified — load-bearing for
  Finding 1.
- `vortex_stdlib.cpp:1953` — `core_.arena().textOf(sym) == symName` — the `d.vars` content
  match. Exact.
- `arena_manifold.cpp:78-105` — `ensureDimension`: `dimensionNamed` → `arenaDims_` cache →
  mint + `d.dims` link. Find-or-mint, exact.
- `compiler.cpp:769-772` — `dimConstant = literal(int64(findOrMintDimension(name)))`.
  Exact.
- Supporting cites rechecked in passing: `vortex_vm.cpp:102-114` (`getOpcodeKind`
  map-then-`asInt64` decode), `compiler.cpp:160-181` (`binding()`'s render check +
  `externalReferences_`), `:407-417` (`validName` refuses `$`), `:475-483` (anchor
  resolution through `bindings_`), `:705-707`/`:853-855` (`Let` value copy / `localCells_`).

## Ruling-adoption verification

- **R14-A — landed in form, failed in substance.** The agent-legs restatement is at
  :2134-2143 as assigned; the two-root vocabulary is correctly invoked ("§6.6's vocab names
  the two-root split" — :1522 is inside §6.6). But the named surviving case is
  mechanically impossible — see Finding 1.
- **Vocabulary sweep — landed.** Glossary `closed` row (:137), `closed` arms (a)/(b)/(c)
  (:1851-1865), the frontier enumeration (:1880-1883), `reify`'s external-capture sentence
  now reads slot *positions* (:1176-1178), (d)'s hand-off names the paired donor slot's
  promoted content (:2214-2217), and the 1.13 entry's "capture edges" was rewritten
  (:3603-3606). The `cap$N`-link usages in `closed` are *decode* usages — the frontier reads
  the wing-edge link to the slot and decodes its endpoint through `masterOf` — which is
  correct there (see Finding 1 for why the same phrase is wrong in the classification
  walk).
- **All adopted minors verified in place:** program-side `+d.name` carve-out — arm (c)
  :1859-1862 plus the §6.1 frontier note :1171-1173 and the re-verify's "under (b) or (c)"
  :1888-1889; the `*handleTarget` (d)-time read — :2225 (`Int64(*handleTarget)`),
  :2241-2243 (in-session `int64` / `*handleTarget` at (d), donors `OpHandle`-kind until
  (e)); recipe tuples obey one-record-one-edge — :1137-1141; the changelog program-side
  arm — :3583-3584 now separates `+d.name →` dim-name/`ensureDimension` from
  `int64`/`*handleTarget`; §10.2 (v) narrated + cite fixed — :2859-2863 reads
  `store.cpp:123`, and the 1.13 entry gained the arm at :3610-3613; declared-victim bound —
  :1234-1237; grafted-twin replay caveat — :1244-1248 and the 1.13 entry's matching
  correction :3599-3601; "no session resolver" mirror — glossary :136 and §6.1's
  replay sentence :1240; `documentId_` ctor-or-load — :634-637, :1562-1564, :2787-2789.

## New findings (v1.14 deltas only)

### Finding 1 — MAJOR

- **Location:** :2138-2139 (§6.6, genesis (a), the R14-A restatement); echoed verbatim at
  :3604-3605 (the "corrected" 1.13 entry) and :3618-3620 (the 1.14 entry).
- **Problem:** the restated reachability names a cross-root edge the stated walk cannot
  traverse — on two independent grounds, plus a category error. "a §5.3 `cap$N` capture slot
  resolving to its binding cell through `masterOf`/`d.clone` is the surviving case" fails
  because: (i) the `program()` walk is outbound-following over an enumerated dim vocabulary
  (:1506-1517) that **does not list `d.clone`** — the walk cannot traverse it at all;
  (ii) even if it could, the needed hop is `-d.clone`: `masterOf` = `cloneMaster` =
  `rankEnd(NEG)` (`arena_manifold.cpp:1009-1012`; the rank runs master →+d.clone→ clones,
  `vortex_core.cpp:338-339`, `vortex_vm.cpp:819-822`), so the only real edge runs
  **binding(state)→slot(program)** — backwards for a program-rooted walk; (iii)
  `masterOf`/`wingOf`'s operand lambda (`vortex_core.cpp:356-365`) is an **operand decode**,
  not an edge — the same non-edge carrier the sentence itself excludes for ref-capture
  records ("slot *content* the link walk cannot follow" is exactly why a `masterOf` decode
  cannot count either). No in-vocab `+d` edge into an agent-`d.vars` cell exists anywhere:
  the `+d.values`-may-name rule takes no edge where `-d.values` is claimed, and a `d.vars`
  binding's `-d.values` is live by construction (:1154-1155). Read as the *agent-rooted*
  walk instead, the sentence still fails: the binding cell enters via its own `±d.vars`
  rank edge — no cross-root edge needed — and the `+d.clone` hop reaches the *slot*, not
  the binding. This is the third iteration of the same defect class (v1.12 `+d.captured` →
  v1.13 "capture edges" → v1.14 `cap$N`/`masterOf`): the *conclusion* is again right —
  the legs classify `>E_ops`/state-side — but the mechanism is again fabricated.
- **Evidence:** walk definition :1506-1517 (+d only; `d.clone` absent; `d.dims` the single
  named exclusion); `masterOf` = `cloneMaster` = `rankEnd(NEG)`
  (`arena_manifold.cpp:1009-1012`, `vortex_core.cpp:344-346`); clone direction master
  →POS→ clones (`vortex_core.cpp:338-339`, `vortex_vm.cpp:819-822`, `zzcore.hpp:63-67`);
  §5.3's own account — the slot is the clone *of* the binding, decoded by the operand
  lambda (:822-824).
- **Concrete fix:** restate as unreachability — the legs classify state-side because no
  `+d`-in-vocab edge reaches them at all: "an agent-`d.vars` binding cell hangs on the
  agent cell's own `±d.vars` rank and this walk never reaches it — the only edge a capture
  creates is the binding cell's own `+d.clone` to the fragment's `cap$N` capture slot,
  which runs state→program on a dim the walk does not read, while the slot's `masterOf`
  decode is a content-side lookup the link walk cannot follow, like the ref-capture's
  `int64`; short of a hand-minted session `Link` — which would classify state-side
  anyway — the legs are unreachable, hence `>E_ops`/state-side, and no agent-rooted
  second walk is owed (its only consumer is `closed`'s frontier enumeration, which *does*
  read the wing-dim slot links and decode them through `masterOf`)." Apply the same fix to
  the two changelog echoes (:3604-3605, :3618-3620).

### Finding 2 — NIT

- **Location:** :2239-2241 (genesis (e) coverage arm, v1.14 edit).
- **Problem:** the sentence derives the copy's ref-box slots "from the regenerated
  ref-capture records" but then cites "a ref-record's `+d.ref` slot" — `+d.ref` lives on
  the `+d.refs` *ref-record*; the ref-capture record's correlation dim is `+d.refFor`
  (both name the same slot). The two record kinds are swapped mid-appositive; the doc is
  elsewhere careful to keep them distinct (:1143-1144, :2216).
- **Fix:** "the paired records' `+d.ref`/`+d.refFor` identify *which* cell's referent is
  captured" — one word pair.

## Non-findings verified (delta claims that hold)

- **`closed` arm (a) is coherent under the cap$N decode model:** the cap$N endpoint is the
  master (binding cell), which is link-reachable from the agent cell via `+d.vars`
  without program-side transit — matching "captures of declared fields and member vars
  … are state-side" (:1868-1870); an artifact-initial (program-side) master falls into
  the refused class on every arm (:1862-1865). The `masterOf` usage here is an *endpoint
  decode* inside the frontier enumeration — legitimate, unlike Finding 1's walk claim.
- **Arm (c) is consistent with P16's marker rule:** a `+d.name` on the `+d.refs` record
  makes it a name-record and skips the marker ceremony (:1046-1051, :1859-1862); the
  dimConstant slot keeps a REF-record so it is marked/relocated, while the paired
  ref-capture record's `+d.name` rebinds the *dim* through `ensureDimension`
  (:1166-1169). No contradiction.
- **The recipe-tuple conditional cannot re-invite M14:** shared tuple endpoints are
  routine (an op cell is `to` of one tuple and `from` of the next) and each takes the
  `+d.values` edge only where its `-d.values` is unclaimed; claimed endpoints ride the
  field's `int64` content for the old→new map to translate (:1137-1141). Sequential
  emission makes the occupancy test deterministic.
- **(d)'s `*handleTarget` read is consistent end-to-end:** donors arrive `OpHandle`-kind
  carrying promoted targets (`arena_manifold.cpp:1947-1951`, doc :2234-2236); the copy's
  slots arrive pre-rehydrated `Int64` with the identical bits for the external arm
  (:2247-2252); the record supplies pairing (`+d.refFor`) and class only (:2214-2217).
- **Carryover observation (out of delta scope, no finding filed):** :1690-1692's
  "capture-linked binding cells sit behind program-side anchors" was not in the sweep
  list and is ambiguous under the new model (a field binding promotes via its own
  `d.vars` path regardless of the program-side wing) — defensible only read as
  frame-bound capture cells or the slots themselves.

VERDICT: NOT CONVERGED
