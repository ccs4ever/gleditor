# r10-writer — convergence critique of `design/vagent-metaprogramming.md` v1.9

Scope: verification of r9-writer's R9-1/R9-2, the "two reasons" residual and remaining nits
against v1.9 (3,002 lines, `9fc8c9c7` → `11fec218`, +292/−134), plus whole-document `E_ops`/
comparand coherence and a new-defect sweep of the deltas (:11, :373-375, :601-622, :970-971,
:1057-1060, :1114-1122, :1157-1158, :1310-1312, :1379-1500, :1558-1561, :1577-1582, :1624-1633,
:1691-1694, :1716-1721, :1795-1887, :1896, :1923, :1961-1964, :2390-2412, :2428-2443, :2549-2551,
:2854-3002).

## Verdicts on r9 findings

| R9 | Verdict |
| --- | --- |
| R9-1 `ops.nodes` for the `applyOpsSegment` seed | **Fixed.** :602-605 and :1379-1383 now name "the artifact's **sealed `ops` segment** (`sealedOpsName`, `provenance.hpp:286`), applied into the fresh store by `historyFromSeal`" with `applyOpsSegment` correctly demoted to "its file-local per-segment step" gating on the `\x7fXSO\x01` magic at `:1033`, and `ops.nodes` explicitly named the different serialization that fails the gate. Every cite verified: `sealedOpsName = "ops"` at `provenance.hpp:286`; `sealedOpsMagic` at `publication.cpp:971`, the `starts_with` gate at `:1033`; `applyOpsSegment` inside an anonymous namespace at `:1030`; `historyFromSeal` the public seam at `:1094`; the `ScrollId` remap (`record.op.span.scroll = at->second`) at `:1086` inside the cited `:1048-1086`. |
| R9-2 P16 "spawn store parameter" | **Fixed — reconciled to supply-side injection.** §10.2 preamble :2430-2431: "`spawn` binds it by **base-injection** before `initGenesis` ... — supply-side, not a store parameter"; P16 row :2443 "base-injected at `spawn` and supplied or verified at `resume`" and its Where column repeats "spawn-side base-injection ... supply-side not a store parameter"; §6.6 :1407-1416 narrates the `WorkerContext` construction before the `initGenesis` freeze; §10.1 :2394-2401 prescribes it; §13 :2549-2551 and §4.4 :373-375 agree. One API shape everywhere now. |
| "two reasons that meet at copy-inheritance" header | **Fixed.** :1872-1873 "The sweep runs last for two reasons — coverage first, ordering second." The changelog's "(coverage, ordering)" naming (:2982) now matches the body. |

**Remaining nit bundle — all landed except the carried `:215` shorthand:**

- `[0..opsTotal)` → `1..opsTotal` at both sites (:620, :1424) — correct per
  `sealableOps`'s `sinceExclusive+1 ..= size` iteration (`publication.cpp:983-985`).
- `mergeValues` order → `(contentHash, authorKey, parent)` (`mesh_value.cpp:519-522`) with the
  fresh destination scoped to `mesh_host.cpp:509-510` vs the `job.base` arm `:507` — all three
  cites verified exactly.
- "(the §6.5 exemption covers them)" → "is what leaves them writable" (:1849).
- `+o:` arm's unit named `+<off-line>:<off-col>` (:1923).
- §5.6 *no program lineage* diagnostic stated (:970-971, inside §5.6 :911-975) and cross-referenced
  from (a) (:1834).
- `+d.refs`-diagram eviction xref relabelled in the changelog (:2940-2943); the fix at :1010-1011.
- `mesh_jobs.cpp:303-318` → `:304-318` (:609, :1387, :1831, changelog :2857) — verified: the
  base/empty-store block is :304-317 plus the `evaluate` hand-off at :318.
- `spawn()` signature cites `mesh_actor.cpp:119-122`/`mesh_actor.hpp:132-135` — verified
  (signature :119-122; declaration :132-135).
- `home` claim's weight moved to the prune — `arena_manifold.cpp:1755-1757` verified exactly
  (the `!holdsOwn` `continue`).
- `program()` walk's root named at (a) (:1822).
- Link-emission cite moved to `setLink` + the `real` identity map (:1466-1469) — mechanism right,
  two line-number wobbles in it (nit bundle below).
- Anonymous-handler gap extended to durable replace/remove (:1577-1582); `learn_handler`
  additive-not-deduped (:1120-1122); `patch:<n>`'s `<n>` pinned to the `d.patch` ordinal with
  `patch:` refused as a user `name` (:1114-1117, consistent with §6.7's reservation :1947-1948).
- `code_named(L)` unresolved-at-emission convention stated (:1624-1633) and self-consistent with
  the role-split rebind at :1680-1686.
- `label_of`'s `cells"…"` acquisition path named (:1060, :1961-1964) — but see **R10-1**: one of
  the two example paths anchors the wrong cell.
- `plan_named` row stops claiming declared handlers (:1057) — now "learned handler clauses
  included; declared handlers carry no `+d.name` to match (§6.6, spec V15)".
- `opcodeMap_` registration widened to the spawn-time image bind (:1310-1312, :1843-1845) —
  consistent between §6.5 and (b); see **R10-3** for the overreach in how the freeze is stated.
- Canonical post-remap comparand named (:1413-1415, :1810-1811); `documentId` stated as a
  construction choice with `restorePublication`'s precedent (:611-615, :1388-1394 — verified
  `store.hpp:132-133` two-arg ctor, `:1435` `documentId_`, `publication.cpp:1128-1129`
  `Store(readerPermascroll, pub.storeId)`; `historyFromSeal` does mint a bare `Store` at :1097).
- R9-A: genesis (a) rewritten to the two landmarks — `E_ops` bound + per-cell classification +
  mandatory ops-segment comparand (:1801-1836); `arena_manifold.cpp:2010-2017` ("Links last"
  comment :2010), `vortex_core.cpp:119-126` (`exportSymbol` closes with `+d.values` then the
  `d.vars` rank link), `store.cpp:3836-3940` (`adoptOpRecords`' topo-sort with the
  `produces` min-heap tie-break, `cmp` at ~:3910), `store.hpp:677-684` `cellRefOf`,
  `manifold.cpp:434`/`:427-442` `birthOp = opIndex` — all verified.
- R9-B: base-injection stated end-to-end; `vortex_core.cpp:14` (ctor runs `initGenesis`),
  `initGenesis` :29-72 adopting `*base->home()`/base dims only when a base exists (:32-44),
  `vortex_core.hpp:268`,`:282`,`:287` (`private:`/`initGenesis`/`home_{noCell}` — exact),
  `arena_manifold.cpp:274-287` (`attach` installs `base_`/`store_` only when unset :279-284),
  `arena_manifold.hpp:269-271` (`store()` public, `base_->store()` fallthrough),
  `manifold.hpp:486` (`Store *store() const noexcept` non-const return),
  `mesh_value.hpp:68-74` (`publishValue(Store&, …)`), `mesh_actor.cpp:260-269` (private store in
  `checkpoint`), `:270-271` (`WorkerContext(checkpoint->manifold.get())`), `worker_context.hpp:52-55`
  (two-arg ctor signature), `mesh_actor.cpp:473-477` (`executor_` invocation), `worker_context.hpp:63`
  (`arena` member) — all verified.
- R9-C: copy pre-rehydration restated as capture-record endpoints (:1855-1857, :1874-1881),
  consistent with §6.1's recorded-template rule (:1104-1106) and its `+d.values`-endpoint encoding
  (:1095-1097).
- Carried: `grant->programRef == continuation->programRef`, `mesh_actor.cpp:215` (:618, :1432
  `:199,215-216`) still cites the `!=` refusal arm for the `==` predicate — shorthand, unchanged.

## Whole-document coherence

**`E_ops` is uniform; bare `E` only in history.** Every live-site band reference is
`E_ops`/`1..E_ops`/`≤ E_ops`/`> E_ops` (:608, :622, :1386, :1412, :1420, :1425, :1427-1435,
:1451, :1719, :1798-1828, :1839, :1896, :2403). The remaining bare `E`/`1..E`/`0..E` are all in
changelog entries narrating what earlier versions asserted (:2859-2860 v1.7, :2905/:2924 v1.8,
:2957-2958 the v1.9 entry quoting v1.8's conflated test) — correct as history.

**Canonical-comparand phrasing consistent.** "canonical post-remap form (op kind plus non-scroll
fields)" appears with the parenthetical at its §6.6 definition sites (:1413-1415, :1810-1811) and
bare at :1719, :2403, :2962, :2975; §4.5 keeps the position-stable framing ("same op sequence at
`1..E_ops` plus resolvable scroll names, not identical stored bytes") which the comparand
operationalizes. "position-stable, not byte-identical" uniform (:606, :1383, :1830, :2393,
:2911); the only standalone "byte-identical" is v1.7's historical claim (:2858), explicitly
corrected at :2911-2914.

**Changelog v1.9 narrates correctly.** The entry (:2955-3002) covers R9-A's landmark split,
R9-B's base-injection + `store()`-already-public + `documentId` choice + canonical comparand,
R9-C's endpoint restatement, and the minor batch — each item checks against the delta; two
cite wobbles ride inside it (`:2035`,`:1864-1867`, nit bundle) plus one unclear label
("the `d.code`-band's `1..opsTotal` arm", nit bundle).

**"Design draft. Nothing here is implemented."** — :5 unchanged, still accurate.

**Mechanicals.** `mdl -i` clean (exit 0). 22 fence markers = 11 balanced pairs. 7 table blocks
(metadata, §1 status, glossary, §4.5 boundary, §6.1 builtins, P15/P16 register, §11 refused).
All 16 TOC anchors resolve; headings sequential §1–§15 + appendix. Register "Used by" columns
consistent (`M10, §6.5, §7.2` / `M11, §4.5, §6.6`).

## New findings (v1.9 deltas only)

**R10-1 MINOR — `cells"##/+d.code"` anchors the wrong cell.** §6.1's `label_of` row (:1060) gives
"`cells"##/+d.code"` for a registered head" as the path that yields the head `Cell`. `##` is the
*arena home cell* (vagent spec §8.7: "`cells"##/d.items"` reads home's `d.items` rank"), while
`d.code` is the rank **on the agent cell** — this document's own words: "the top-level ranks
themselves — `d.code`, `d.patch`, `d.synapse` on the agent cell" (:1538-1539) and "`d.code` is the
agent's registration rank" (:1346); `home` and the agent cell are distinct cells the sideness
invariant depends on (:1486). `cells"##/+d.code"` traverses `+d.code` off `home` — a rank that does
not exist there — and yields `[]`. The doc already names the right anchor: `agent_cell()` is "the
`cells"…"` anchor for state ranks" (:1052), and the `d.code` rank is one of them. **Fix:**
"`cells"$a/+d.code"` for a registered head (`a := agent_cell()`)" — matching the `$`-anchor
convention the second example (`cells"$clause/+d.spin"`) already uses.

**R10-2 MINOR — "`WorkerContext(base, reader)` … resume's own injection shape" misattributes the
call site.** §6.6 says the spawn arm takes "resume's own injection shape: ... constructing
`WorkerContext(base, reader)` (`mesh_actor.cpp:270-271`, `worker_context.hpp:52-55`)" (:1407-1409),
repeated at :1717, :2397-2398, :2430-2431 ("the shape `mesh_actor.cpp:270-271` uses at `resume`"),
the P16 row's Where column, and the changelog (:2966). The cited call is
`std::make_unique<WorkerContext>(actor->checkpoint->manifold.get())` — **one argument** — with the
reader installed separately afterwards at :272
(`context.arena.setDefaultSpanReader(actor->checkpoint->store.get())`). The two-arg signature is
real (`worker_context.hpp:52-55`, correctly cited), and passing the reader at construction is
arguably the better spawn shape (`core(*arena.setDefaultSpanReader(reader))` installs it before
`initGenesis`, where resume installs it after) — but it is not "the shape resume uses". An
implementer checking the cite finds a one-arg call where the text promises two. **Fix:** "the
`WorkerContext` ctor (`worker_context.hpp:52-55`) — resume passes `base` alone and installs the
reader via `setDefaultSpanReader` (`mesh_actor.cpp:270-272`); the spawn arm supplies both at
construction."

**R10-3 MINOR — `opcodeMap_` swept into the `initGenesis` freeze.** §6.6 :1405-1406 says a
post-construction `attach` leaves "`core.home()`, the session dims and `opcodeMap_` anchored to
the base-less arena for the whole first incarnation", and the changelog :2968-2969 repeats "the
`home_`/dims/`opcodeMap_` frozen at `vortex_core.cpp:14`,`:29-70`". `initGenesis` never touches
`opcodeMap_` — it is `VortexVM` state populated by `VortexStdLib`'s `mintOpcode` calls during
`WorkerContext` construction (`vortex_vm.cpp:75`,`:99`; stdlib ctor `vortex_stdlib.cpp:1436+`),
whose mints are ephemeral cells on either base shape, so nothing in the map is "anchored to the
base-less arena" at all; and the missing *image* entries are what genesis step (b) re-registers —
an adapter pass a post-`attach` genesis could still run. The unrecoverable freeze is `home_` and
the session dims alone. Companion looseness: "empty on every fresh context" (:1310-1311, :1843-1844)
is literally false — the map holds the ~100 freshly minted stdlib ops; the meant reading is "empty
of the folded image's op cells" (v1.8's "empty after `resume`" had the same shorthand; the v1.9
widening repeats it). **Fix:** drop `opcodeMap_` from the frozen list — "`home_`, the session dims
— and `opcodeMap_` empty of the image's op cells until (b) runs" — or scope the freeze claim to
`home_`/dims and name (b) as the image-map remedy either way.

**Nit bundle (v1.9 delta text):**

- :1467 and changelog :2987 — `setLink`'s cite `arena_manifold.cpp:2035` lands inside the
  base-linked skip-check; the call is `store.setLink(out.version, real.at(arena), …)` at
  `:2041-2042`. The disposition's own "~:2035" was approximate; the adopted figure is off.
- :1468 and changelog :2987 — the `real` identity-map cite `:1864-1867` stops one line short of
  the mechanism: `real.emplace(arena, arena)` is `:1868` (the `if (!isEphemeral(arena))` opens
  :1867).
- :1810, :2975 — "`CompactOp` bytes legitimately differ under the `ScrollId` remap": no `CompactOp`
  type exists — `CompactOpNode` (`compact_op.hpp:24`, which the doc itself uses at :605/:1382) on
  the store side, `Op`/`OpRecord` (`ops.hpp:318`, `binary_ops.hpp:86`) on the segment side; the
  remap rewrites `record.op.span.scroll` (`publication.cpp:1086`), so `OpRecord` is the tighter
  referent.
- :1873-1876 — "the regenerated ref-records' **capture endpoints**" misattributes the schema:
  ref-records carry `+d.ref→slot`, "never +d.values" (:1009-1011); the endpoint-as-cell encoding is
  the *recipe* records' `+d.values` (:1095-1097) — which the very next clause correctly describes.
  (d) at :1855 already says "capture-record endpoints"; (e)'s lead noun should match ("the
  regenerated recipe records' capture endpoints").
- :603, :1381, :2392, :2984 — the `historyFromSeal` cite `publication.cpp:1093-1099` stops one
  line short of the `applyOpsSegment` call at `:1100` — the range covers the signature and
  `addScroll` but not the per-segment apply it exists to make.
- :2998 — changelog's "the `d.code`-band's `1..opsTotal` arm" names no doc object; the second
  `1..opsTotal` site is §6.6's lineage bullet (:1424). "the §6.6 lineage bullet's arm of the same
  correction" would be checkable.
- :1310-1311, :1843-1844 — folded into R10-3 above: "empty" for a map holding the fresh stdlib
  mints; "empty of the folded image's op cells" is the accurate phrase.

VERDICT: CONVERGED — both r9 minors landed (R9-1's sealed-`ops`/`historyFromSeal` correction is
verbatim-verified, R9-2's supply-side base-injection is uniform across §4.4/§6.6/§10.1/§10.2/P16/
§13/changelog), the "two reasons" residual and every adoptable nit are in, `E_ops`/comparand/
changelog/mechanicals are coherent end-to-end, and the v1.9 deltas introduce no blocking or
major defects — three minors (`##`-anchored `d.code` path, the `(base, reader)` resume
attribution, the `opcodeMap_` freeze bundling) plus a six-item cite-and-label nit bundle.
