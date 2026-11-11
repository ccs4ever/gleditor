# Impl-plan round 1 disposition — v1 → v2

Tally: **0/7 converged.** zigzag 1B/5M, codebase 7M, architect 3B/3M, ergo 2B/5M,
meta 2B/6M, vortex 3B/6M, writer 1B/6M. The findings converge hard — the same
defects were found independently across lanes, which makes the rulings easy.
All spot-checked `file:line` cites are clean; every defect is structural
(wrong phase boundary, inverted order, unowned work), not citation drift.

## Blocking clusters

### I1 — genesis ordering stated inverted (6 lanes: zigzag B-1, codebase M4, architect M-1, ergo B2, vortex B1, writer B-1)

The plan's phase-5 bullet reads "(e) name-record rebind … (f) `d.patch` replay
after rebind". Spec §6.6 is unambiguous: **(d) `d.patch` replay, then (e) the
`+d.refs` rebind sweep last**, (f) is "turns" (:2346, :2381, :2435-2439). The
v1.7 R7-B reorder exists precisely so rebinds see post-installation symbols.
The plan's wording implements the misbind/drop class it claims to test.

**Ruling:** fix the bullet to spec letters and order: (d) `d.patch` replay →
(e) `+d.refs` rehydration sweep last (covering registered heads *and*
replay-installed copies). Audit the doc for any other echo of the inversion.

### I2 — `std:meta` surface lands after its consumers (4 lanes: architect B-1, meta B2, vortex B3, ergo M1, +writer M-6)

Phase 3 ships `run`, `reflect`/`reify`, five resolvers — all `std:meta`
members — but the package machinery (import allowlist, member-signature
table, prelude linkage) sits in phase 4, so nothing in phase 3's exit is
reachable through the language.

**Ruling:** split the member surface by consumer (the plan's own read/write
boundary): phase 3 gets the `std:meta` scaffolding — allowlist entry,
signature-table rows, prelude skeleton — plus the *read* group (`run`,
`reflect`/`reify`, resolvers, `value_of`, `module_named`/`test_of`/`body_of`);
phase 4 gets the *mutation* verbs.

### I3 — phase-1 checker work has no body producer (meta B1)

`parseCodeBody` lands in phase 2, so phase 1's checker cannot compute `T`,
purity, or arity, and the `SourceLocation`-keyed fragment table has no
producer in any phase (phase 2's expand bullet omits emitting it).

**Ruling:** land a skeleton `expand` in phase 1 — body-parse + seeded resolve
+ fragment-table emission, all splices/`static` uniformly refused — which
also plants `check(program, table)`'s signature with its producer. Phase 2
then fills the real stage. Also: `apps/vagent/main.cpp:46-53` calls
`Parser`+`check()` directly — rewiring through `frontend` is a phase-1 item,
and `code_path.cpp`/`expand.cpp` join `VAGENT_FRONTEND_SRCS` in the phase
that creates them (the explicit list at Makefile:688 — the `cell_path`
precedent the plan itself cites).

### I4 — phase 5↔6 boundary cuts through genesis, not transport (3 lanes: architect B-3, vortex B2, ergo B1)

Phase 5's "genesis re-run without transport" exit needs a folded `1..E_ops`
image band — lineage seed + span-source binding + filing + (a)–(e) — all
assigned to phase 6 or nothing. The plan's own rule ("nothing checkpoint-
surviving lands before the lineage-store and filing seams exist") is violated
by its own phase map.

**Ruling:** cut the boundary at *transport*. Phase 5 ("durability and
genesis") gets the lineage-store construction, `historyFromSeal` seeding,
span-source binding, dim filing, the genesis (a)–(e) driver with the I1
ordering, verb-replayed dedupe — all locally constructible and testable
without wire transport. Phase 6 keeps only the transport-side seams: spawn
base-injection, `resume` take-or-verify, segment-apply surface, `documentId`
arm, and the P16 flag/re-kind/restore ceremony + P4 wake.

### I5 — silent external preconditions (3 lanes: architect B-2, ergo B1, vortex M-cluster, writer M-5)

The spec's status line says "the actor executor does not [exist]";
`vagent-implementation.md` shows iterations 4–9 open. Phases 3–7 presuppose
it at three depths: `run` needs the `#Call` frame ABI (owed §11.1 work — the
kernel's `localCells_` is the interim no-call shape), `std:meta` needs the
linked-prelude path, P16 needs the adapter's `Executor` callback. No phase
builds or names this substrate.

**Ruling:** add a preconditions section listing the owed substrate (actor
executor, `#Call`/frame ABI, V10 dims, P4 `wake(pid)` — unimplemented at
`mesh_actor.hpp:81`), and give each phase an external-requirements line
marking each exit kernel-simulable vs adapter-gated.

## Major rulings

### M-A — the unowned inventory (5 lanes: zigzag M3/M4, codebase M6/M7, architect M-2, meta, writer M-4)

Work consumed by the phases but produced by none:

- `programRef` bind record (`E_ops` + recorded instruction enumeration)
  emission — consumed by phase-4 P15, phase-5 genesis, phase-6 comparand.
  Owner: phase 3 (emission contract) — the record is emitted alongside
  `compileFragment`/`+d.refs` marking.
- Genesis (b) `setOpcodeKind` pass needed *at spawn*, not only post-resume
  (architect): state both arms in the P15 bullet.
- Genesis (c) + the adapter bind set — program root, `agent_cell()`, the
  data-minted apparatus dims (`d.code`/`d.patch`/`d.entry`/`d.patch-head`/
  `d.validate`/`d.refs`/`d.ref`/`d.recipe`/`d.refFor`/`d.mesh.relocation`/
  `d.weight`/`d.synapse`), `validate` constants (§6.1 :1053-1060). No phase
  mints or binds them. Owner: phase 3's adapter plumbing + phase 6's filing
  list must include "the bind set above" (zigzag M2 — the plan dropped
  §10.1's fifth list member).
- The genesis (a)–(f) ceremony driver hook (§1's priced "std:agent genesis
  hook"). Owner: phase 5.
- Dropped verbs/functions: `learn_where`, `learn_body` (phase 7's §9.2 exit
  requires `learn_where`), `module_named`/`test_of`/`body_of`,
  `set_weight`'s full dispatch, `staticFuel` (`ScalarCompilerLimits`,
  spec :3070-3072, confirmed absent `compiler.hpp:23-26`), dead-`let`
  erasure (§9.1/§9.4 residue), reserved-name refusals (`splice`/`static`/
  `run`/`value_of` decls, §5.1), the descriptor `closed`-flag computation
  (emitted phase 3, consumed phase 4, computed nowhere), no-marks
  dim-minting discipline (`reify`'s `ensureDimension` arm can write
  `arenaDims_` under live marks — vortex).

**Ruling:** add an explicit per-phase work-item for each; where an item is
emission-contract (programRef record, `+d.refs`/`+d.recipe` marking schema)
it belongs to phase 3 — the marker-edge *mint* stays phase 6.

### M-B — phase-4/5 `learn_*` split undefined (meta M, writer M-2, ergo M2)

`learn_*` writes `d.patch` records, mints `patch:<n>`, registers on `d.code`
*at learn time* (§6.6:1854-1855); `unlearn` reads `d.patch` — all session-
scope machinery the plan files under phase-5 "durable" concerns.

**Ruling:** repartition — session mint/record/register + `unlearn` read-side
→ phase 4; monotone serial, no-ABA, alias separation, verb-replayed dedupe,
genesis ordering → phase 5. State the split explicitly.

### M-C — phase-6 filing order + vocabulary (zigzag M2/M3, architect M-3)

Sub-steps place the lineage ceremony before filing but never place the pin
itself; filing after base-injection lands outside the sealed fold
(spec :3031-3039). Emit-side filing (before the compile arena's
`initGenesis`) is ownerless. And "19 system dims" is the r18 mislabel again —
`:50-69` mints **17** dims; 19 is cells-all-told incl. the `:35`/`:44`
`home`/`d.dims` pair (ergo).

**Ruling:** restate the sub-order with the pin as an explicit step —
mint/file → fold → pin/inject → first-resolution — on both the emit and
spawn lineages; fix the count to 17 system dims + the genesis pair.

### M-D — test-harness honesty (ergo, vortex)

`vagent_runtime_test` filter-outs the vagent dir (Makefile:698);
`make test` never runs the `vagent_*_test` binaries; `tests/xuzz/` glob →
`xuzz_test` is the only auto-discovered kernel-linked home.

**Ruling:** the cross-cutting section names the actual test homes and says
plainly which phase exits run under `make test` vs a manual binary; add a
test-infrastructure work item (a `vagent` test suite or filter fix) owned
by phase 1.

## Minor bundle (apply all)

Stamp-field ordering note (phase-1 `ast.hpp` field consumed by phase-0
`cloneExpr` — acceptable since the field is additive, but say so);
P16 "emission marking" in phase 6 reworded to "marker-edge mint — the record
schema is phase-3's emission contract" (zigzag m6); stale "v1.18" companion
cite → v1.19 (writer); `mdformat --wrap 100` compatibility (writer m11 —
run it or note the exemption); phase-3/4 resolvers' reads of `run`-target
head-vs-op discriminator belong with the read group; any remaining
lettered-step echoes of the I1 inversion.

## Expected v2 shape

| Phase | Title                              | Key repartition |
| ----- | ---------------------------------- | --------------- |
| 0     | extractions                        | + Makefile wiring all listed files |
| 1     | literal + skeleton expand + driver | + fragment table emission, `main.cpp` rewiring, reserved names |
| 2     | full expand                        | + table already produced, `staticFuel`, dead-`let` |
| 3     | fragment emission + read surface   | + `std:meta` scaffolding/read group, programRef+E_ops emission, bind set, P16 record schema |
| 4     | mutation + P15                     | + session `learn_*`/`d.patch`/`patch:<n>`/`d.code`, spawn+resume `setOpcodeKind` |
| 5     | durability incl. fold-back genesis | + lineage seed, filing, (a)–(e) driver in spec order |
| 6     | transport seams + P16 ceremony     | pin placed; marker mint + windows only |
| 7     | plasticity exercises               | + `learn_where` named for §9.2 |
