# Round-10 disposition — orchestrator rulings on v1.9 findings

0 blocking, 5 majors, ~14 minors. Two lanes converged (meta, writer). The R9-A landmark fix
verified end-to-end in three lanes — the mechanism holds; the majors are now narrow
specification gaps, except one genuinely deeper seam (architect's span-source finding).

## Majors — all adopted

**R10-A — the `patch:` name reservation contradicts §9.3 and leaves `patch:<n>` unmintable**
(ergo + codebase, independently). `:1115-1117` refuses `patch:` as a user `name`, but
`learn_plan("Feed","patch:near_water",…)` (:2286) supplies exactly that — and `patch:<n>`'s
assigner is unspecified (the ordinal belongs to the patch the call creates, so only the verb can
mint it). **Ruling:** `learn_*` mints `patch:<n>` itself — `<n>` = the `d.patch` ordinal the call
creates; the user `name` argument is an alias *recorded alongside*, never the `+d.name` label.
`patch:` is reserved for the verb's minting only. §9.3's example corrected (`"patch:near_water"`
becomes the minted label or an alias — state which).

**R10-B — genesis (b) never identifies the op cells it re-registers** (vortex). "Every op cell
in the `1..E_ops` band" has no discriminator — an `asInt64 ≤ Linked` scan would misregister
`dimConstant`/ref cells holding int64 ≤47 into `opcodeMap_`, after which P15 refuses honest
writes. **Ruling:** the op-cell set is the *recorded instruction enumeration* — the artifact
records its op positions at emission (the `mesh_evaluator.cpp:658-663` precedent: ops are known
by construction, not discovered by value). `setOpcodeKind` applies to exactly the recorded set.

**R10-C — `historyFromSeal` never installs the span source** (architect — the deeper seam).
`historyFromSeal`/`applyOpsSegment` skip `setContentSource`/`bindPublishedLocalScroll`
(cf. `restorePublication` `publication.cpp:1129-1131`), so every artifact-local span — all
`d.*` dim names, `+d.name`/`src:` labels, symbol names — resolves to `""`; `initGenesis` mints
ephemeral twin dims and every name-resolution path dead-reads. **Ruling:** the seed ceremony is
`restorePublication`'s full shape — `setContentSource` + `bindPublishedLocalScroll` — not the
bare fold. Named alongside the owed supply-side seam. Companion minor: `historyFromSeal` mints
its own `Store()` with no id parameter (`applyOpsSegment` is file-local) — the
adopt-`documentId` arm is unmechanized; state it as fresh-mint-and-record or name the owed
parameter. Companion nit: `resume` already transports a caller store (`ReceivedValue checkpoint`
— `mesh_actor.cpp:188`,`:261`, dropped after `userPermascrollPtr()`) — narrow the "expose
neither today" phrasing.

**R10-D — `+d.values → referent` contradicts the unified `+d.refs` schema** (zigzag). The
schema says "`+d.ref`, never `+d.values`" (:1009-1011,:1740-1742); R9-C's capture-endpoint
phrasing minted the wrong edge and wrongly extended it to name-records (d.code-invariant
violation). Also `*handleTarget`-equality holds only for capture/external referents — internal
referents rebind through the old→new reify map to copy twins (vortex minor agrees). **Ruling:**
capture endpoint = the record's `+d.ref` slot's referent (recipe records — writer's nit);
internal referents derive through the old→new map, `*handleTarget` for external only.

**R10-E — comparand operand is ambiguous** (codebase + vortex minors promoted). "The artifact's
seeded segment" (serialized record order) vs the seeded store's fold (`adoptOpRecords` topo
order) — on a branched image band the literal reading refuses an honest resume. **Ruling:** the
comparand is the adapter's *adopted store's* `1..E_ops` band, decoded post-`adoptOpRecords` —
canonical order-vs-order comparison, never bytes-vs-records.

## Minors — all adopted

- **`cells"##/+d.code"` anchors arena `home`, not the agent cell** (meta + ergo + writer — three
  lanes): `d.code` hangs on `agent_cell()`; the path yields `[]`. Fix to `cells"$a/+d.code"` off
  `agent_cell()` (:1060, :1961-1964, §9.3 idiom).
- **`WorkerContext(base, reader)` misattribution** (writer): resume passes one arg +
  `setDefaultSpanReader` (`mesh_actor.cpp:270-272`) — state the real call shape.
- **`opcodeMap_` not frozen by `initGenesis`** (writer): only `home_`/dims are — scope the
  claim.
- **(a)'s named `>E_ops` legs unreachable from `program()`** (zigzag): the agent cell is a
  distinct `stateRoot` never `+d`-reached from program cells — the walk-vocab enumeration must
  root at the agent cell separately or drop those legs.
- **Non-image-ingress overstatement** (zigzag): a `>E_ops` twin is admitted, not detected —
  state both arms (persistent `≤E_ops` vs `>E_ops`/ephemeral) in one clause.
- **Genesis-resolvable vs never-resolvable `run` operands share one refusal** (ergo): name the
  emission-time distinction (fail-until-resume vs fail-forever).
- **`unlearn` LIFO can't target same-named learned handlers** (ergo): state the limit.
- **Citation/cleanup nits**: `setLink` `arena_manifold.cpp:2041`; `real.emplace` `:1868`;
  `CompactOp`→`OpRecord`/`CompactOpNode`; `historyFromSeal` cite `publication.cpp:1100`; head-
  less slot's observable content; emission-fixed slot genesis-only fill — apply all.

## Round status

v1.10 to follow. Trajectory: r8 1M → r9 1B+4M → r10 0B+5M — counts oscillate as each fix is
verified one layer deeper, but every major is now a *specification* gap in named mechanisms, not
a new architecture. The span-source find closes the last unreachable-object seam in the chain.
