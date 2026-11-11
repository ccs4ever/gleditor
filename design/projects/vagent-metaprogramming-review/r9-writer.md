# r9-writer — convergence critique of `design/vagent-metaprogramming.md` v1.8

Scope: per-finding verdicts on r8-writer's R8-1..R8-4 + 13-item nit bundle against v1.8 (2,844
lines, `3feb0065` → `9fc8c9c7`, +271/−100), plus whole-document coherence of the store-ownership
reframe (R8-A) and a new-defect sweep of the deltas (:10, :94, :357-361 spec-§4.4 supersession,
:373-378 M11, :596-620 §4.5 reframe, :1003 xref, :1050-1051 `plan_named`/`code_named` rows,
:1143-1150 §6.2, :1361-1445 lineage + `home`, :1503-1528 descriptor arms, :1563-1639 name-records,
:1646-1698 P16 preamble + pump, :1732-1796 genesis (a)–(f), :1826-1886 §6.7, :2139 `#Call`+spill,
:2295-2309 adapter block, :2325-2336 §10.2 + P16 row, :2412-2417, :2442-2450 §13, :2566-2574 U-M11,
changelog :2747-2844).

## Verdicts on R8 findings

| R8 | Verdict |
| --- | --- |
| R8-1 "(step (e) above)" mislabel | **Fixed.** :1639 reads "(step (e) below)" — correct: the (a)–(f) enumeration sits at :1732-1796, below the reference. |
| R8-2 `patch:` under "emitter's automatic prefixes" | **Fixed.** :1854-1857: "The label prefixes `src:`/`reify:` and the `patch:` clause-name convention are **reserved at module declaration**" — `patch:` is now the clause-name convention beside the label prefixes, exactly the proposed subject. |
| R8-3 descriptor clause arm | **Fixed.** :1503-1509: the convention now has two arms — qualified `[<module>:][<Agent>.]<member>` for program-original targets, "the **verbatim `patch:<n>` clause label** for learned-clause targets — … never a canonicalized `<module>:<Agent>.patch:<n>` the `plan_named` match would then miss". Stronger than proposed: also names the graft-verb parent (`goal`/`event` name for replay attach) and states the declared-handler anonymity gap (:1520-1524, spec V15). |
| R8-4 content-named symbols | **Fixed.** :1626-1628: "the shape restriction that keeps the qualified strings clause `+d.name` labels carry on the same dim from matching; symbol names need no such guard, they live as cell *content*, never on `+d.name`". |

All 13 nits: **12 landed, 1 partially.**

- dequeue cite — **Fixed** :1688: "`mailbox.front()`/`pop_front()` at `:467-468`" (verified:
  `mesh_actor.cpp:467-468` is exactly `front()`/`pop_front()`).
- `sealLocalSpool` call-site pair — **Fixed** :613-615 and :1385-1387: `mesh_value.cpp:227-228`,`:241`
  beside `publication.cpp:611-617`,`:664-672` (all four cites verified; see below for the
  `[0..opsTotal)` wording that rides on them).
- `getOpcodeKind` rationale — **Fixed** :1758-1764: map *membership* is now the stated consumer
  (§6.5's write-guard `opcodeMap_.contains(masterOf(cell))`); `getOpcodeKind`'s `asInt64` fallback
  verified at `vortex_vm.cpp:106-111`.
- `home` direction — **Fixed** :1437-1441: the invariant is now "about *paths*, not one edge
  direction: no link path runs from the agent cell to `home`, the `+d.cursors` rank or any `d.stack`
  cell" — correct under promote's either-direction walk (`vortex_vm.cpp:157-160`,`:206` verified:
  `rankTail +d.cursors → cursor`, `cursor +d.stack → frame`).
- changelog `:1952-1954` narrowing — **Fixed** :2783: `arena_manifold.cpp:1934-1954`.
- "name-record sweep" shorthand — **Fixed** :2763: "(e) the `+d.refs` rehydrate/rebind sweep last".
- `home` walk direction, `(link eviction, §4.3)` xref, `Call`+spill, `(e) "last"`, "below `E`",
  "sideness test (below)", "classifies prefix-less", "r7-B" — all **Fixed** (:1437-1441, :1003
  `arena_manifold.cpp:1347-1377 / M14`, :2139 `#Call` frame + spill, :1775 "last genesis pass",
  :1733-1734 positional `≤ E`, :1444-1445 "the `closed` account below", :2571/:1872 "a cell outside
  the bound lineage", :2765 "R7-B"). The vacuous `patch:near_water` `code_named` example is replaced
  by `demo:Forager.f` (:1853) — still a string no label can carry, but now a plausible user input
  making the right point (verbatim treatment of a qualified-looking string); resolved.
- unchronicled edits — **Fixed** :2841-2844: the memo keyless-record sentence and the v1.6
  `module vars`→`member vars`/`cap$N` rephrase are chronicled.
- **copy-inheritance "two reasons" — partially applied.** :1784-1785 keeps "The sweep runs last for
  two reasons that meet at copy-inheritance" while renaming the reasons "First, coverage" / "Second,
  ordering". Ordering (rebinds must see post-installation symbols) is not a copy-inheritance reason —
  the header still mislabels, and changelog :2822 now correctly names "(coverage, ordering)", making
  the body's header inconsistent with its own changelog. Residual nit, carried.

## Whole-document coherence of the v1.8 reframe

**"Lineage store" terminology is consistent.** `lineage-store seam` (:373, :1649, :1683, :2326,
:2443), `the adapter's own lineage store`/`adapter-kept lineage Store` (:94, :1409, :2413),
`the store lineage itself` (:599), `shared base lineage` (:356, :595, :2774), `bound programRef
lineage` (:1872, :2570-2571), `prefix-less lineage` (:1412, :1747, :2770) — "lineage" uniformly names
the ops-prefix chain, "lineage store" the adapter-owned `Store` object carrying it. `position-stable,
not byte-identical` is now uniform (:603, :1368, :1744, :2297, :2804); the only "byte-identical"
left is v1.7's changelog entry (:2751), correct as history and explicitly corrected at :2807.

**Changelog narrates correctly.** The 1.7 entry was corrected in place where the disposition asked
(`mesh_value.cpp:227-228`,`:241` + `:664-672` cites :2755-2756, sweep renamed :2763, `R7-B`
capitalized :2765, `:1934-1954` :2783, qualified-run phrasing :2776-2777) while keeping faithful
records of what 1.7 itself claimed (byte-identical :2751, innermost :2786) — each now matched by an
explicit correction line in the appended 1.8 entry (:2800-2844). The 1.8 entry's per-item account
verifies against the diff: R8-A ownership, position-stable seeding, the `plan_named`/`symbol_named`
arm drop, `patch:<n>` descriptor arm, declared-handler gap, positional (a), (d)→(e) install-set
hand-off, the `OpHandle`-kind-copy correction, nested-literal direction, `code_named` fallthrough,
and the writer/nit batch — all present and accurate.

**"Design draft. Nothing here is implemented."** — :5 unchanged and still accurate.

**Mechanicals.** `mdl -i` clean (exit 0). 22 fence markers = 11 balanced pairs. 7 tables (metadata,
§1 status, glossary, §4.5 boundary 6-col, §6.1 builtins, P15/P16 register 6-col, §11 refused 2-col).
All 16 TOC anchors resolve; headings sequential §1–§15 + appendix. Register "Used by" columns
consistent (`M11, §4.5, §6.6`).

**New-code cites verified on the delta:** `attach(Space)` installing `base_`/`store_` when unset
(`arena_manifold.cpp:274-287` — `attach` at :274, installs :278-284); `applyOpsSegment` at
`publication.cpp:1030` with the `ScrollId` remap (:1048-1069, `:1082`) and `adoptOpRecords`;
`sealableOps` scroll table :981-1008; `sealLocalSpool` `opsAlreadySealed` machinery :611-617 +
segment record :664-672; `spawn()` accepting no store `mesh_actor.cpp:156`; `resume()`'s private
`checkpoint->store` :260-269; `ActorCheckpointView`/`Executor` signatures `mesh_actor.hpp:102-106`,
`:113-116`; fresh-`Store` publish `mesh_actor_resume_test.cpp:125`; `mergeValues`
`mesh_value.cpp:512-545` + `MeshHost::commit`'s fresh-store arm `mesh_host.cpp:484-530` (:505-507);
`mesh_actor.cpp:467-468` dequeue; promote's verbatim handle-target arms `:1950`,`:1996`;
`initGenesis`/`home_` cites unchanged-valid.

## New findings

**R9-1 MINOR — `ops.nodes` names the wrong file for the `applyOpsSegment` seed.** :601 and :1367
both say "the artifact's **sealed ops segment** — the `ops.nodes` the toolchain emits once per
artifact, applied into the fresh store by `applyOpsSegment` (`publication.cpp:1030+`)". `ops.nodes`
is the *native spool* — 64 KiB `OpsSegmentHeader` + raw `CompactOpNode[]`
(`segmented_ops_spool.hpp:99-125`, `store_loader.cpp:108-117`), the format `xudu-dump` reads. The
sealed ops segment is a different serialization — `sealedOpsName = "ops"` (`provenance.hpp:286`),
`\x7fXSO\x01` magic + varint scroll table + `OpRecord`s (`publication.cpp:971`,`:1010`) — and
`applyOpsSegment` throws on anything not starting with that magic (`publication.cpp:1033`), so an
`ops.nodes` file fed to it fails at the first check. The mechanism described is the right one
(position-preserving segment, `adoptOpRecords`); only the filename is wrong. **Fix:** "the sealed
`ops` segment (`sealedOpsName`, `provenance.hpp:286`) the toolchain emits once per artifact" — or, if
the native spool is the intended seed, swap the mechanism to the spool-load path and drop
`applyOpsSegment`.

**R9-2 MINOR — the spawn-side seam has two unreconciled API shapes.** §6.6 :1375-1377 resolves the
gap adapter-side: "the adapter's seeding is the §10.1 step that owns the store, installed into the
arena by `attach(Space)`" — the adapter reaches `arena` through the `Executor`'s `WorkerContext&`,
so no `spawn` signature change is implied; §10.1's comment agrees ("`spawn()` takes no store today,
`mesh_actor.cpp:156` — this step owns that gap"). The P16 register row instead names a runtime API
change: "the lineage-store seam (`spawn` store parameter, `resume` store supply/verify)" (:2336
"Where" column). A `spawn` parameter and adapter-side `attach` are alternative shapes of the same
seam — an implementer cannot tell whether `MeshActorRuntime::spawn`'s signature changes or the
adapter attaches through the context it already holds. **Fix:** pick one ("`spawn` gains a store/`Space`
parameter that lands via `attach`", or "no `spawn` change — the adapter attaches through
`WorkerContext`"), or mark them explicitly as alternatives pending the API design.

**Nit bundle (all in v1.8 delta text):**

- :614 and :1386 — "the sealed segment covers `[0..opsTotal)`" is off by one at both ends: ops are
  1-indexed (`sealableOps` iterates `sinceExclusive + 1 ..= segmentedOps().size()`,
  `publication.cpp:983-985`), so `opsAlreadySealed = 0` seals `1..opsTotal` — the disposition itself
  wrote `[1..opCount]`. `[0..opsTotal)` includes the reserved slot and drops the last op.
- :1738 — "`mergeValues` promotes imported twins into a fresh store in discovery order": the order is
  a canonical sort on `(contentHash, authorKey, parent)` (`mesh_value.cpp:522-526`), not discovery
  order. The claim needs no order at all — any consistent order still fails the positional test.
- :1784-1785 — the "two reasons that meet at copy-inheritance" header retained over renamed
  (coverage/ordering) reasons; see the partial-fix entry above.
- :1050 — "`plan_named` … matches every clause `+d.name` label verbatim, event-handler clauses
  included" over-reads alone: declared handlers mint no `+d.name` (:1521-1524), so only learned
  (named) handler clauses are in the domain. §6.6 corrects the picture; "named handler clauses
  included" would be self-sufficient.
- :1763 — "(the §6.5 exemption covers them)": "covers" is ambiguous between *applies to* and
  *protects* — the point is the exemption is what leaves them writable.
- :2836-2838 — changelog names the corrected xref "the focus-slot hazard's `(link eviction, §4.3)`";
  the fix lives in the `+d.refs` diagram's genesis-rewrite slot line (:1003), a record-cell eviction
  context, not a focus-slot one.
- :215's `grant->programRef == continuation->programRef` cite shows the pin predicate; the code line
  is the `!=` refusal arm (`mesh_actor.cpp:215`) — same invariant, shorthand only, noted for
  completeness.

VERDICT: NOT CONVERGED (minor level) — every r8 finding landed (one nit partially: the
copy-inheritance header), the lineage-store reframe is coherent end-to-end and the changelog narrates
accurately, but the delta introduces two new minors: the `ops.nodes`/`applyOpsSegment` format
mismatch (R9-1) and the `spawn`-parameter-vs-`attach` seam-shape ambiguity (R9-2), plus a 6-item nit
bundle led by the `[0..opsTotal)` off-by-one. No majors; defect surface still contracting.
