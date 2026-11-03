# Round-7 disposition — orchestrator rulings on v1.6 findings

Four lanes converged in substance (ergo, codebase, zigzag; writer at minor level): **0 blocking,
3 majors, ~14 minors + nits**. The three majors collapse to one root — the recorded-extent
discriminator has no mechanized store lineage — plus one adjacent ordering defect. This is the
loop's deepest finding: it questions the checkpoint model itself, not just the wording.

## Majors

**R7-A — name the store lineage: the artifact ops ride the base spool's low prefix (vortex-major,
architect-major, zigzag-minor-1 — three lanes, one root). Adopted.** The persistent manifold
arrives pre-folded and const inside `receiveValue` (`rebuildManifold`, `mesh_actor.cpp:260-271`,
`manifold.cpp:427-442`), so genesis cannot sequence "image first" — and persistent refs are
per-store op indices, so `0..E` ordering exists only if the checkpoint spool *contains* the image
as its low prefix. That topology already has precedent: `job.base`-style base lineage — the
program image is a **shared base-lineage segment** the agent's store prefixes onto, not a
per-agent-published range. Ruling:
- State the lineage: the program image lives as the artifact store's low ops prefix, anchored at
  load; every checkpoint of the agent inherits the prefix position-by-construction (checkpoints
  append, `sealLocalSpool` covers the whole spool — `publication.cpp:611-617`). `E` = the image
  prefix length, recorded in the adapter's `programRef` bind record.
- Reconcile "program cells do not checkpoint": they are not *published per agent* — they ride the
  shared base lineage the checkpoint prefixes onto. Amend the §4.4/"transient" rows to say
  "not republished", not "not in the spool".
- Prefix maintenance is an invariant, not an assertion: the adapter guarantees `publishValue`
  runs against the store lineage that contains the prefix; a prefix-less publish (foreign ingress,
  sibling manifold) yields an empty program-side set and the discriminator **fails closed**
  (refuses `learn`, does not silently admit state→skeleton links — fixes architect's
  "fails open" minor in the same clause).
- Byte-identical image prefix is required for positions to be stable (zigzag-2): state that the
  artifact's ops segment is loaded verbatim, and that `mintOpcode` re-registration must run a
  `setOpcodeKind` pass over the folded image rather than re-minting (vortex minor — re-running
  `mintOpcode` under the folded model registers the wrong cells).

**R7-B — genesis (e) name-record sweep must follow (f) patch replay (meta-major). Adopted.**
Calls naming patch-installed/replaced symbols resolve pre-replacement bindings under the current
order. Ruling: reorder genesis — (a) image/lineage, (b) `ensureDimension` find-or-mint +
`opcodeMap_` re-registration (the `setOpcodeKind` pass), (c) `programRef` bind, (d) `d.patch`
replay, **(e) name-record sweep last** so rebinds see post-installation symbols; state why the
naive pre-(f) position was wrong (copy-inheritance interacts with replayed installs — name the
dependency rather than swap blindly).

## Minor batch — all adopted

- **`run`-operand records discriminate by slot role, not name shape** (vortex-m): a qualified
  `plan_named`/`symbol_named` `run` operand on a head-valued slot must write `int64(head)` — the
  writeback key is the *record's role* (call-operand vs run-operand), with `+d.name` shape the
  resolution path only. A `run` of a head-less program `def` symbol is a distinct refusal class —
  name it.
- **`src:`/`reify:`/`patch:` reservation contradiction** (writer R7-1, meta-m): reserve all three
  prefixes at module-declaration (a module may not be named `src`, `reify`, or `patch`); delete
  the :1477-1479 "a module literally takes that name" arm.
- **`plan_named` module-split vs verbatim clause `d.name`** (ergo R7-F1): clause `+d.name` stores
  the qualified string verbatim — `plan_named` resolves by verbatim match on clause `d.name`,
  not by module `d.vars`; unify :1068/:1669/:1782.
- **`home` post-resume** (codebase): `initGenesis` adopts the base's persistent home
  (`vortex_core.cpp:30-36`, `manifold.cpp:450-451`) — fix "arena-minted and ephemeral".
- **`ForeignOpHandle` in P16's promote clause** (codebase-n): "links, `OpHandle`, and
  `ForeignOpHandle`" (`arena_manifold.cpp:1952-1954`).
- **Genesis (b) rationale** (writer R7-2): the `opcodeMap_` repoint rationale belongs to the
  empty-per-`WorkerContext` account — move it off "stale indices".
- **`d.name` rank-dim claim** (writer R7-3): `d.name` is a single-link label dim, not a rank —
  fix the "bare `d.` = rank dims" claim.
- **M6 "run emits a `#Call`" vs head-valued operand** (writer R7-4): the `#Call` op's operand is
  the head-valued slot; reconcile M6/§11 wording with §6.2's construction.
- **code_named foreign-literal hole** (ergo R6-F2 residual): state explicitly that foreign
  literal heads (other artifacts' `code"…"` heads) have no re-resolution spelling this round —
  deferred to U-M, named not implied.
- **`+o:c` nested-literal label arm** (ergo F4 residual): add the origin clause — nested literals
  mint `src:<module>:<line>:<col>` of their *innermost* site plus clone ordinal.
- **Empty-set arm fails closed** (architect-m): folded into R7-A above.
- **Step (b) fold narration** (architect-m): `rebuildManifold` inside `receiveValue` does the
  fold; genesis narrates it as a bind, not an overlay.
- **Nit bundles** (writer/vortex/architect/ergo/meta residual): P16 row head/op hop restored;
  capture-wing doubling; dangling "pre-head convention"; unattributed pump-ordering claim →
  changelog; `src:` shorthand vs `#label` override; `code_named` operand record-key ambiguity —
  apply all.

## Round status

v1.7 to follow. The root cause is identified and the fix direction is three-lane-convergent;
expect next round to close.
