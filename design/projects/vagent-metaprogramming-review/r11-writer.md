# r11-writer — convergence critique of `design/vagent-metaprogramming.md` v1.10

Scope: verification of my r10 minors (R10-1 `cells"##/+d.code"`, R10-2 `WorkerContext` call
shape, R10-3 `opcodeMap_` freeze, plus the cite/type nit bundle), the r10-disposition minors I
own as writer/coherence lane (`unlearn` LIFO limit, two-root walk vocab, non-image-ingress
arms, fail-until-resume vs fail-forever split), and a new-defect sweep of the v1.10 deltas
(`11fec218` → `3f9731da`, +169 lines, 28 hunks: :136, :601-631, :1074-1075, :1127-1145,
:1330-1341, :1388-1405, :1410-1477, :1521-1526, :1586-1608, :1614-1622, :1683-1698,
:1738-1742, :1782-1798, :1880-1911, :1921-1941, :1965-1984, :2380-2404, :2489-2528,
:2543-2558, :2561-2564, :3086-3171).

## Verdicts on r10/r10-disposition items

| Finding | Verdict |
| --- | --- |
| R10-1 `cells"##/+d.code"` wrong anchor | **Fixed.** :1075 now reads "`cells"$a/+d.code"` over the registered rank, `a := agent_cell()` — `d.code` hangs on the agent cell, not arena `home`, so `##` reads an empty rank" — self-consistent with :1067 (`agent_cell()` = "the `cells"…"` anchor for state ranks") and :1597-1598 ("`d.code`, `d.patch`, `d.synapse` on the agent cell"). The `[Cell]`→`Cell` indexing step is named. |
| R10-2 `WorkerContext(base, reader)` misattribution | **Fixed, verbatim-verified.** Doc now states the real shape at all four sites (:1461-1464, :1783-1788, :2509-2513, :2546-2549): "the `WorkerContext` two-arg form/ctor (`worker_context.hpp:52-54`) — `resume` itself passes `base`/the manifold alone and installs the reader post-construction via `arena.setDefaultSpanReader` (`mesh_actor.cpp:271-273`); the spawn arm supplies both at construction." Verified: the two-arg ctor signature is `worker_context.hpp:52-54` (`explicit WorkerContext(const Manifold *baseManifold = nullptr, const SpanReader *reader = nullptr)` :52-53); `resume` mints `std::make_unique<WorkerContext>(actor->checkpoint->manifold.get())` at `mesh_actor.cpp:270-271` and calls `context.arena.setDefaultSpanReader(actor->checkpoint->store.get())` at :273 — the cite `:271-273` covers both exactly. |
| R10-3 `opcodeMap_` in the freeze | **Fixed.** :1454-1457 now scopes the freeze to "`core.home()` and the session dims", adding "`opcodeMap_` is not `initGenesis`-frozen at all — it is VM state populated by `VortexStdLib`'s `mintOpcode`s during `WorkerContext` construction, and the *image* entries it lacks are what genesis step (b) re-registers." Verified: `initGenesis` (`vortex_core.cpp:29-70`) touches only `home_`/`dims_`; the stdlib ctor (`vortex_stdlib.cpp:1436`, `mintOpcode` calls from :1504) populates the map during `WorkerContext` member init. The "empty" overstatement is also fixed at both sites: "`holds none of the folded image's op cells`" (:1333-1334, :1936-1937 — the map does hold the ~100 stdlib mints, and the new phrase is the accurate one). Changelog :3085-3088/:3159-3160 narrates it correctly; live `P15`/`§6.5` agree. |
| `setLink` `arena_manifold.cpp:2035` | **Fixed.** :1524 cites `:2041` — `out.version = store.setLink(out.version, real.at(arena), …)` is at `:2041-2042`. Changelog :3169 agrees. |
| `real` identity map `:1864-1867` | **Fixed.** :1524 cites `:1868` — `real.emplace(arena, arena)` is `:1868` (the `if (!isEphemeral(arena))` guard opens :1867). |
| `CompactOp` → `OpRecord`/`CompactOpNode` | **Fixed.** :1884 now compares "canonical post-remap `OpRecord`/`CompactOpNode` fields (op kind plus non-scroll fields)" — both are real types (`ops.hpp:318`, `compact_op.hpp:24`); the only remaining bare `CompactOp` is the changelog's own fix narration (:3169). |
| `historyFromSeal` range `:1093-1099` | **Fixed.** All sites cite `publication.cpp:1093-1103` (:603, :1413, :2492), which now covers the signature (:1093-1096), the `Store()` mint (:1097), `addScroll` (:1098), the `applyOpsSegment` call (:1100) and the return. Changelog :3170-3171 names the reason. |
| R10-C span-source seam | **Fixed.** The hydration pair `setContentSource` + `bindPublishedLocalScroll(self)` is named at all three seed sites (:606-613, :1415-1420, :2493-2496) as "`restorePublication`'s own pair at `publication.cpp:1129-1131`" — verified: `history->setContentSource(&source)` :1129, `history->bindPublishedLocalScroll(self)` :1131, the `Store(userPermascroll, pub.storeId)` precedent :1127-1128. The dead-read mechanism is accurate: `historyFromSeal` installs no span source (:1093-1103), `applyOpsSegment`'s remap only re-ids scrolls (`remap.emplace(localScroll, selfScrollInHistory)` :1049, `record.op.span.scroll = at->second` :1086), so `baseDimensionNamed`/`initGenesis` miss folded dim names. The adopt-`documentId` arm is correctly named **unmechanized** with the real reasons — `historyFromSeal` mints `Store()` internally (:1097), `applyOpsSegment` sits in an anonymous namespace (:1023-1091, file-local), `Store()` mints its own `UserPermascroll` (`store.cpp:123`) — and owed as "`a `historyFromSeal` overload or a `restorePublication` seed`" alongside the supply-side seam, uniform across :621-631, :1427-1435, :2498-2504, :3141-3143. |
| R10-C `resume` transport narrowing | **Fixed.** The "neither exposes a store" claim is narrowed at all four sites (:1440-1445, :2551-2553, :2564, :3144-3146): `ReceivedValue checkpoint` does transport a caller `Store` (`resume` signature `mesh_actor.cpp:182-186`, param :184; `ReceivedValue::store` is a `unique_ptr`, `mesh_value.hpp:101`), the store's only member read is `checkpoint.store->userPermascrollPtr()` at :261, and the rebuilt store comes from `receiveValue`'s `verified` product (`:260-269`) — "the owed change is trusting or comparing it, not new transport" is accurate. One line-number wobble inside it — see nit bundle. |
| R10-D capture endpoints | **Fixed.** :1966-1977 now splits the schema correctly: "a ref-record's `+d.ref` slot identifies *which* cell's referent is captured, while the referent endpoint itself rides the *recipe* record (the recipe tuple's `+d.values`)" — matching §6.1's schema (:1023-1032: `+d.ref→`slot, never `+d.values`; :1110-1112: recipe records' `+d.values` → endpoint cells). The internal-vs-external split ("internal referents rebind through the old→new reify map to copy twins; `*handleTarget` equality holds for the external arm only") implements the ruling. |
| R10-E comparand operand | **Fixed.** :1880-1888 names it the "**ops-record order comparand**": the adapter decodes its *adopted lineage store's* `1..E_ops` band (`adoptOpRecords`-folded `OpRecord`/`CompactOpNode` sequence, canonical order, `store.cpp:3836-3940` deterministic) against the received store's decoded band pairwise — order-vs-order, never bytes-vs-records. Consistent with :1468-1470 and the P15-adjacent restatement. |
| R10-A `patch:<n>` minting | **Fixed — with one new defect in its wake (R11-1).** The mint is uniform: glossary :136 ("the verb-minted `patch:<n>`", "the user `name` alias"), §6.6 :1129-1135 and :1589-1593 (patch payload gains the alias field), descriptor verbatim arm :1617-1621, §6.7 reservation :1740/:2044, §9.3 example :2383-2385 + prose :2399-2402, §10.2/changelog :3126-3131. §9.3's `learn_plan("Feed","near_water",…)` correctly passes the alias, never the label. **But see R11-1**: two consumers of name-matching were not updated for unique minted labels. |
| R10-B recorded instruction enumeration | **Fixed.** :1921-1935: `setOpcodeKind` applies to "exactly the recorded instruction enumeration" on the `programRef` bind record, `mesh_evaluator.cpp:658-663` precedent verified (`program.instructions` loop :659 → `setOpcodeKind` :663); the value-scan counterexample names `dimConstant` operand cells (`vagent/compiler.cpp:769-772` verified — the lambda mints `literal(int64(findOrMintDimension))` cells); §6.5 :1336-1341 and the P15 row :2563 agree. |
| Two-root walk vocab | **Fixed, phrasing uniform.** §6.6 :1401-1405 "the vocabulary spans **two roots**: `program()` and `agent_cell()` are distinct adapter-bound roots … never `+d`-reached from each other"; genesis (a) :1904-1908 "distinct adapter-bound roots never `+d`-reached from each other (§6.6's vocab names the two-root split) … agent-side named legs need the second walk rooted at the agent cell, or they simply classify nothing" — consistent wording, consistent consequence. |
| Non-image ingress arms | **Fixed.** :1908-1911 names both: "a persistent ref `> E_ops` or any ephemeral ref is state-side, so an image twin minted above `E_ops` is admitted and reads as state, while a non-image cell inside `1..E_ops` is what the comparand just excluded." |
| `run` operand refusal split | **Fixed.** :1688-1698: unwritten slot → observable content `None` (real `ValueKind`, `ops.hpp:155`), no sentinel, descriptor deref fails cleanly; "**emission-fixed**" (a later `learn` registering `L` does not fill the slot); **fail-until-resume** vs **fail-forever** distinguished. |
| `unlearn` LIFO limit | **Stated but defective — see R11-1.** |

## Whole-document coherence

- **`patch:`/`patch:<n>` terminology**: "verb-minted"/"the verb's own mint"/"the learning verb
  minted" is uniform across live sites (:136, :1130, :1139, :1590, :1618, :1740, :2400, :3127);
  "alias" is uniform for the user `name`. Two carried phrasings lag the minting — nit bundle.
- **`E_ops` uniform**: every live-site reference is `E_ops`/`1..E_ops`/`≤ E_ops`/`> E_ops`; bare
  `E`/`1..E`/`0..E` appears only in the appendix narrating earlier versions (:2948, :2980-2981,
  :2995, :3026, :3045, :3076-3079) — correct as history.
- **"Design draft. Nothing here is implemented."** :6 — accurate; status unchanged and still true.
- **Mechanicals**: `mdl -i` clean (exit 0); 22 fence markers = 11 balanced pairs; 7 table blocks
  (metadata :3-10, §1 status :78-105, glossary :122-143, §4.5 boundary :591-596, §6.1 builtins
  :1064-1091, register :2561-2564, §11 refused :2575-2604); all 16 TOC anchors resolve against the
  16 `## ` headings.
- **v1.10 changelog narrates correctly**: every claimed change is in the diff — R10-A mint +
  §9.3 correction (:3126-3131), R10-B enumeration (:3132-3136), R10-C hydration + unmechanized
  arm + resume-transport narrowing (:3136-3147), R10-D endpoint split (:3147-3150), R10-E
  comparand (:3150-3153), and the minor batch (:3154-3171). Two nit-bundle items ride inside it
  (:3143, :3145).

## New findings (v1.10 deltas only)

**R11-1 MAJOR — §6.6's "same-named" reasoning is falsified by the mint it sits beside**
(:1140-1145). The new text says: "`learn_handler` … a second same-named handler clause *appends*
beside the first, so `plan_named(name)`'s first-hit stays ambiguous between them … and `unlearn`
… matches by recorded name, so among same-named learned clauses it always pops the *latest* patch
recording that name — the clause-cell argument cannot pick an earlier twin out of order."

Under v1.10's own rules the described cohort cannot exist and both behavioral claims are false:

1. **The cohort is empty at label level.** `learn_*` mints each clause's `+d.name` as `patch:<n>`
   with `<n>` the new patch's ordinal (:1130-1132, :1589-1591) — ordinals are unique per patch,
   and `patch:` is refused as a user `name` (:1133), so no two learned clauses ever share a
   `+d.name`. The doc itself states the consequence two lines earlier: "a learned clause's minted
   `patch:<n>` is never re-hit" (:1138-1139). "Same-named" survives only as same-*alias* — and
   the alias is not a `+d.name`.
2. **`plan_named(name)`'s first-hit is never ambiguous between them.** `plan_named` matches
   clause `+d.name` labels verbatim (:1073, :1627). Two same-alias handlers carry distinct labels
   `patch:3`/`patch:7`: an alias lookup ("foo") misses both, a label lookup hits exactly one.
   There is no ambiguous first-hit under the only specified matching. The carried sentence was
   true in v1.9 (user `name` sat on the label verbatim, so two `learn_handler` calls could mint
   colliding labels); R10-A made it false and the text did not follow.
3. **The `unlearn` claim asserts the opposite of the specified mechanism.** `unlearn` resolves
   the argument "by the reverse read — the clause's `+d.name` label cell" and undoes "the latest
   patch whose recorded target name equals it" (:1643-1646, :1089; the recorded target name for a
   learned-clause target is the clause's verbatim `+d.name` = `patch:<n>`, :1617-1619). So
   `unlearn(earlier-twin-cell)` resolves `patch:3` and pops patch 3 — the earlier twin **is**
   individually unlearnable, in any order, precisely because labels are unique. The only
   mechanism under which "cannot pick an earlier twin" holds is matching by the recorded *alias* —
   which (a) the doc never states `unlearn` consults, (b) the argument cannot supply anyway since
   resolution yields the label not the alias (:1644-1645), and (c) would make learned clauses
   un-unlearnable rather than LIFO-bound. The real LIFO cohort is patches sharing one recorded
   *target* name — stacked replaces on a program-original clause/symbol or repeated patches on
   one learned clause — where the argument's resolved name cannot distinguish patches.

An implementer faces a fork the text does not resolve: implement the documented mechanism and the
stated limit never fires and is observably false, or implement the sentence and contradict
:1643-1646's explicit machinery.

**Proposed fix** — restate the sentence against the mechanism, e.g.: "`unlearn` matches the
argument's resolved name (the clause's `+d.name` label) against each patch's recorded target
name, so it is LIFO only among patches naming *one target* — stacked replaces on a
program-original clause or repeated patches on one learned clause. Same-`name`-alias learned
clauses carry distinct minted `patch:<n>` labels, so each is individually unlearnable in any
order; `plan_named` likewise disambiguates them by label, and `priority` differentiates them at
handler selection." If instead the intended design is alias-matching `unlearn`, that needs to be
specified as such (arg → recorded alias), and :1643-1646's resolution path revised to match.

**Nit bundle (v1.10 delta text):**

- `:1441`, `:2552`, `:2564`, `:3145` — "`mesh_actor.cpp:182`, consumed at `:186`,`:261`": :186 is
  the `wallNow` parameter's closing line; `checkpoint`'s first touch is the `!checkpoint.store`
  null-guard at `:188` (the r10 disposition itself cited `:188`). If `:182-186` is meant as the
  signature span it should be written that way; as a consumption cite it is off by two.
- `:624`, `:3143` — "`Store()`'s own `UserPermascroll` mint at `store.cpp:123-124`": the mint is
  `:123` (`userPermascroll_(std::make_shared<UserPermascroll>())`); `:124` is `chronofilade_`.
- `:2044` — "the `patch:` clause-name convention" is stale against the mandatory mint: the rest
  of the document says "verb-minted"/"the verb's own mint"; "convention" reads as optional. The
  reservation itself is correctly stated; only the noun lags.
- `:2385` — code comment "// as patch:<ordinal>" uses a one-off placeholder; the document's
  canonical spelling is `patch:<n>` (11 other sites). "`patch:<n>`" in the comment keeps the
  idiom.
- `:606`, `:1415` — "**hydrated** like any restored history" (span-source install) collides
  stems with §6.6's technical "**rehydration**" (:1957+, the `OpHandle`→`Int64` writeback); two
  different operations. "Binds the span source / `restorePublication`'s hydration pair" or naming
  the pair without the verb would keep them distinct.
- `:1339-1340` — "the other wrong shape — the `asInt64 ≤ Linked` value scan §6.6's step (b)
  rules out being the first": the possessive-plus-appositive reads as "the scan … being the
  first" and takes two passes to parse; "— the first wrong shape being the `asInt64 ≤ Linked`
  value scan §6.6's step (b) rules out —" is cleaner.
- `:1463` vs `:2511`/`:2547` — "passes `base` alone" vs "passes the manifold alone" for the same
  argument; `baseManifold` is the parameter name and "base-injection" is the doc's own frame, so
  it parses — listing for uniformity only.

VERDICT: NOT CONVERGED — every r10 minor verified verbatim (anchors, call shapes, freeze scope,
cite ranges, type names, hydration pair, comparand operand all landed), `E_ops`/`patch:`
terminology/changelog/mechanicals coherent end-to-end, but R10-A's unique-label minting left one
self-contradiction in §6.6: the new `unlearn` LIFO sentence and the carried `plan_named`
ambiguity claim describe name-collision the mint eliminated — an implementer cannot satisfy both
the sentence and the specified mechanism (R11-1, MAJOR), plus a six-item cite/phrasing nit
bundle.
