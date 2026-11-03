# r7-writer — convergence critique of `design/vagent-metaprogramming.md` v1.6

Scope: per-finding verdicts on r6-writer's R6-1/R6-2 + nit bundle against v1.6 (2,504 lines, `96ed8103`
→ `bd2b791e`, +170/−63), new-defect sweep of the deltas (:1313-1347 sideness/walk, :1442-1493
writeback split, :1528-1670 genesis + labels, :2103-2106 P16 row, changelog :2476-2504), `mdl -i`.
Cites verified against the tree.

## Verdicts on R6 findings

| R6 | Verdict |
| --- | --- |
| R6-1 `patch:` in the rebind set | **Fixed.** :1486-1487 now reads "A *label* — `src:`, `reify:` or a user `#label`"; the P16 row :2106 drops `patch:` likewise; the three-kind head-label inventory :1635-1643 is unchanged. Remaining `patch:` mentions are the correct clause-name convention (:1068-1069, :1647, :1974) or changelog history (:2461). **But the fix left a contradiction — see R7-1.** |
| R6-2 `plan_named` misdescription | **Fixed.** :1661-1670 splits the two lookups: `symbol_named` reads the resolved module's `d.vars` (spec §5.7, no agent-owner arm) while `plan_named` "matches the qualified `module:Agent.plan` string against clause `+d.name` labels". Consistent with the :1012 table row ("qualified three-part `module:Agent.plan` name") and :1405-1406. The second-layer nit is also resolved — the bullet no longer conflates the two-arg signature with the string grammar. |
| R6-3 `home`/`+d.stack` anchors (question) | **Resolved.** :1341-1347: "`d.stack` hangs off *cursor* cells (`vortex_vm.cpp:206`, `:865-887`), never `home` — the cursors sit on `home`'s `+d.cursors` rank (`vortex_vm.cpp:159-160`), and `home` is arena-minted and ephemeral, not a program-side anchor". All three cites verified literally (`:206` `linked(*cursor, dims().stack,…)`; `:865-889` the cursor→stack links; `:157-160` the `+d.cursors` rankTail append). Frames now stay out of the durable set by *unreachability*, and the prune list :1337-1341 drops the `home` anchors consistently. |
| nit (a) "left dead" | **Fixed.** :2241-2242 "the argument itself left untouched (live session data, not dead)"; :1300 already said "left untouched". |
| nit (b) glossary `closed` | **Fixed.** :137 now carries the run-target label arm ("or, on a `run`-target record, the target head's label") and the two-arm refused class, matching :1413-1422. |
| nit (c) `manifold.hpp` cite | **Fixed — and better than r6's suggestion.** `:1326` now cites `manifold.hpp:154-158`: `ephemeralBit` is :154, `isEphemeral` :156-158. Exact. (r6's proposed `:153-157` was itself off by one.) |
| nit (d) reflect-born label presume | **Fixed.** :1447-1450: "a `code_named`-resolved operand → its `d.name` label — while a `reflect`-born operand is a session-side wrapper with no durable referent, so an opaque or `reflect`-born `run` target in a learnable fragment is a closedness refusal at emission". |

**All r6 findings resolved.** Disposition majors verified in place: R6-A's image-first genesis is
:1574-1605 (a)–(g) with `manifold.hpp:295` `birthOp == CellRef` cited correctly (:295 is the
`birthOp` field's `== this CellRef` comment), the `programRef` bind-record extent carrier at
:1315-1317/:1589-1591, and `importValue`-ingress/sibling-manifold empty-set clauses at :1324/:1581;
R6-B's entry/head writeback split is at :1470-1493, :1085-1088, the §6.1 legend :968-970, and the
P16 row :2106. Minor-batch items verified: `ensureDimension` find-or-mint (`arena_manifold.cpp:78-105`
is exactly `ensureDimension`, `dimensionNamed`→`arenaDims_`→mint), `vortex_stdlib.cpp:4051` is
`makeCell(name)`+`+d.name` link (the fresh-label precedent), `writeValue` re-cited `:196-229` (the
function spans :196-229), `vortex_core.cpp:113-126` covers both `rankTail` rank-append sites.

## New findings

**R7-1 MINOR — the `src:`/`reify:` reservation contradicts itself between two v1.6 additions.**
:1477-1479: "a label's `src:`/`reify:` prefix reaches the label channel **unless a module literally
takes that name** (which degrades to a dropped patch, never a wrong bind)". :1648-1649: "the
emitter's automatic prefixes (`src:`, `reify:`) are label-reserved, **so no module can take those
names**". Both are normative and cannot both hold: if the prefixes are reserved module names the
:1478 caveat and its "dropped patch" degradation are unreachable dead text; if modules may be so
named, :1648's "no module can" is false. Changelog :2492-2493 sides with reservation, so the
:1478 hedge is the likely stale half — but either resolution should also account for `patch:`,
which is *not* reservable (a user-supplied clause-name convention, :1068-1069) and still degrades
through the same path.

**R7-2 MINOR — genesis step (b) keeps the pre-ref-preservation `opcodeMap_` rationale.** :1585-1586:
"then **re-point `opcodeMap_`** … and the overlay carries stale indices into it". Under the new
image-first ref-preserved (a) the program refs are restored at `0..E` — the artifact's op-map
indices are *preserved*, not stale. The real dependency is §6.5's own account (:1253-1255): the
map is per-`WorkerContext` VM state, **empty after `resume`**, repopulated by the program-load mint
— which logically belongs to (a)'s import/replay, not after the overlay. Move the repoint into (a)
or restate the reason.

**R7-3 MINOR — `d.name` fails the new "bare `d.` = rank dims" claim.** :1304-1307: "the bare `d.`
spellings are the *rank* dims, whose membership edges run `rankTail +d → member`". `d.name` is in
the bare list but is a single-link label dim — `vortex_stdlib.cpp:4052-4053` links
`var +d.name → nameCell` singly, initGenesis mints it as a plain dim (`vortex_core.cpp:61`), and
the doc's own memo-key reasoning (:288-289, :294-295) relies on `-d.name` single-occupancy, which a
rank's chain would violate. Drop `d.name` (already covered by `+d.name` in the same list) or narrow
the parenthetical.

**R7-4 MINOR — M6's "`run` emits a `#Call`" predates the head-valued operand.** :238: "`run` emits a
`#Call` to the entry op reached through the head's `+d.entry`"; :255-256 prices "a frame … and a
`Call`"; §11's row :2132 repeats "a same-cursor `#Call`". Read literally — a `Call`-opcode operand —
this contradicts R6-B: `Call` executes `toInt(inputs[0])` (`vortex_vm.cpp:854-856`) and never
derefs `+d.values`, while §6.2 :1085-1088 now specifies a head-valued slot and a `+d.entry`/`+d.values`
deref. A genuine `Call` op aimed at `int64(head)` would jump a non-op cell. Disambiguate `#Call`
(the §5.8 frame convention) from the `Call` opcode.

**Nit bundle:**

- :2106 P16 row — the rewrite *dropped* the head/op discriminator hop the v1.5 row carried ("— then
  the head/op discriminator →"): for `graft_method`/`learn_method` symbols, whose `+d.values` is a
  **head** (:1157-1158), the hop is load-bearing; "walk → `writeValue(slot, int64(entryOp))`" reads
  as if `+d.values` were always the op. Main text :1482-1485 has it right; restore the hop in the row.
- :1337-1340 — "program-side capture wings" now appears in *both* the items list and the anchors
  parenthetical ("…sit behind program-side anchors (…, a program-side capture wing)") — the v1.6
  edit moved it from anchor-only to both slots; the intended item is the cells the wing anchors.
- :1405-1407 — the patch target descriptor is resolved "through the `plan_named` resolution path"
  read off "the clause's `d.name` label" — clause targets only. `learn_method`'s symbol targets
  resolve through `symbol_named`'s module-`d.vars` walk (the name lives on the symbol cell; no
  clause/`+d.name` is involved) — sharper now that §6.7 spells the two-rank split.
- :292 — "§6.1's pre-head convention" is coined with no definition; the nearest text is the
  op-or-head resolution rule at :994-996.
- :1538-1540 — "the pump orders publish ahead of all queued turns while the flag is armed" is a new
  claim with no cite and an ambiguous subject (the adapter's pump loop vs `pumpOne`/mesh_actor —
  the latter would be a second change "outside current code", in tension with :1535-1536). It is
  also absent from the 1.6 changelog, as are the "module vars"→"member vars" rename (:737, :1425)
  and the `cap$N` alias rephrase (:739-741) — none is in the r6 disposition.
- :1447 — "a literal operand → its `src:` label" is wrong for `code"#label …"` literals, whose head
  carries the user's `#label` override (:1639), not a `src:`; the record must carry the head's
  actual `+d.name`. Pre-existing shorthand on a v1.6-edited line.
- :1670 — "Both match labels verbatim" glosses over a representation difference: module/symbol
  cells carry names as *content* (`exportSymbol` mints `makeCell(name)`, `vortex_core.cpp:119-127`,
  no `+d.name` edge) while clause/head labels are `+d.name` label *cells*.
- :1582 — "(§6.6)" self-reference inside §6.6.

## Mechanical checks

`mdl -i`: **clean, exit 0**. 22 fence markers = 11 tagged pairs. All 16 TOC anchors resolve;
heading numbering sequential (§1–§15 + appendix). Pipe counts consistent in all touched tables
(register table 6 cols, glossary 2). Verified cites on the delta: `vortex_vm.cpp:854-856`
(`Call` `toInt(inputs[0])`), `mesh_actor.cpp:260-271`, `:462`, `:473-477`, `:659-662`, `:672`,
`arena_manifold.cpp:78-105`, `arena_manifold.hpp:468-470`, `manifold.hpp:154-158`, `:295`,
`vortex_core.cpp:113-126`, `:119-127` (`exportSymbol`, starts :119), `:196-229` (`writeValue`),
`vortex_stdlib.cpp:4051`, spec §4.3 fields-on-`d.vars` (:386-392).

**Changelog 1.6 accuracy:** every claim verified against the diff except the omissions noted in the
nit bundle (pump ordering, member-var rename, `cap$N` rephrase — all additive, none contradicting a
listed item). Historical entries (:2459-2461's `patch:` in the 1.5 entry) are accurate for their
versions.

VERDICT: NOT CONVERGED (minor level) — all r6 findings resolved with correct cites; 4 new minors +
a 7-item nit bundle in the v1.6 deltas, headed by the `src:`/`reify:` reservation self-contradiction
(R7-1). No majors; defect surface still contracting.
