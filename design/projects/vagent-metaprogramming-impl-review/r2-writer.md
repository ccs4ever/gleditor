# r2-writer — document-coherence critique of `design/vagent-metaprogramming-implementation.md` (v2)

Verdict: **CONVERGED**. Counts: **0 BLOCKING / 0 MAJOR / 3 MINOR / 5 NIT**.

Every r1 item closed — verified line-by-line against v2 and re-verified against the spec and the
tree. Residual is one boundary-labeling imprecision and two small completeness/wording gaps, none
load-bearing. `mdl -i` clean (exit 0); `mdformat --check --wrap 100` now **passes** (m-11 resolved);
every `file:line` cite added in v2 spot-checked clean.

## Per-finding verification (r1 → v2)

- **B-1 — CLOSED.** Letters are right now. Phase 5's driver enumerates in spec order: (a) lineage
  bind (:326-330), (b) `ensureDimension` + `setOpcodeKind` (:331-333), (c) `programRef`→root
  (:334-335), **(d) `d.patch` replay** (:336-344), **(e) `+d.refs` rehydration — last pass**
  (:345-350), and "(f) is turns and stays out of scope" (:324-325). The ordering rationale is
  stated correctly — "runs after (d) precisely so name-record rebinds see post-installation
  symbols (the R7-B ordering, spec :2435-2439)" — verified verbatim against the spec
  ((d) :2346, (e) "last genesis pass" :2381, (f) turns :2440, rationale :2435-2439). The
  verbatim re-mint claim (:341-344) is spec-faithful (:2350-2358: "a fresh label cell written
  with the record's text exactly, never a re-link … drop `patch:<n>` at the *second* genesis").
  No other echo of the inversion remains (grep: no "after rebind").
- **M-1 — CLOSED.** Explicit session/durable split per the M-B ruling: phase 4 owns the
  learn-time machinery (:275-284 — two `reify`s, `d.code` registration, `d.patch` append shape,
  `patch:<n>` mint off `d.vars`, un-minted alias, `unlearn` read side); phase 5's "Durable
  semantics" owns only cross-genesis properties (:351-354 — monotone serial, no-ABA, alias
  separation, two-copy invariant).
- **M-2 — CLOSED.** The `+d.refs`/`+d.recipe` schema is phase 3's "emission contract, stated in
  full" (:191-195 — `+d.name` name-record/ref-record discriminator, `+d.grab→"call"/"run"` role
  labels, `+d.refFor` pairing, `+d.recipe`-member `+d.name` reservation — all verified vs spec
  :2058-2062 and P16 :3110). Name-record arm owned by phase 5's (e) (:345-348); the ref-record
  arm that genuinely needs P16 (`Int64(*handleTarget)`, `OpHandle`→`Int64` restore) owned by
  phase 6 (:386-387).
- **M-3 — CLOSED.** All five twins named (:263-264 — `learn_plan`/`learn_method`/`learn_handler`/
  `learn_where`/`learn_body`, "all five `graft_*` durable twins, §6.1 :1168-1174"); §9.2→
  `learn_where` dependency explicit at :412.
- **M-4 — CLOSED.** Adapter block decomposed: phase 3 owns the `programRef` record's emission
  half (`E_ops` + instruction enumeration, :198-202, spec :3057-3058 verified), the full bind
  set harness-supplied (:216-223 — program root, `agent_cell()`, the twelve apparatus dims,
  `validate` constants, V10 no-marks path — spec :1053-1060 verified verbatim), and the
  `std:meta` member surface is enumerated (16 members matching spec rows :1152-1167 exactly).
  Phase 5 owns (c), the (a)–(e) driver, and §1's priced genesis hook (:323-324).
- **M-5 — CLOSED.** "Preconditions" section (:20-45) names all four owed substrate pieces with
  cites — call ABI (`localCells_` interim no-call shape, `compiler.cpp:853-855` verified),
  adapter/executor, V10 mechanism, linked-prelude path — plus P4 `wake(pid)` owned as a phase-6
  work item (`mesh_actor.hpp:81` non-optional `ActorMessage` verified, `pending(pid)` at
  `mesh_actor.cpp:659` verified, `vagent-agent-language.md:1670` = the P4 row, verified). Every
  phase carries a Requires line marking kernel-simulable vs adapter-gated.
- **M-6 — CLOSED.** `std:meta` scaffolding (allowlist `checker.cpp:2061-2067`, signature-table
  rows `:765-779`, prelude skeleton) + the read group lands in phase 3 with the members that
  consume it (:203-215); mutation surface is phase 4's.
- **Minor bundle — all closed:** v1.19 cite both places (:3, :448); Makefile wiring stated
  per-phase (`code_path.cpp` :106, `expand.cpp` :113-115 — the "lands here, not in phase 2"
  note is the right fix); `staticFuel` joins `ScalarCompilerLimits` (:167, spec :3070-3072
  verified); `setOpcodeKind` pass at spawn *and* resume (:258-260, :331-333, spec :2335-2343
  verified); stamp-field ordering note + named `expand` seam (:86-89, :78-81); `Splice`/`Static`
  uniform refusal + unknown-names interim (:111-113, :160-162); `learn_method` now
  "unlink-then-install" (:283-284, spec :2363-2364 verified); missing cites added (§6.7
  resolver rules, §6.6 refusal class named by class, filing bullet cites); reserved-name
  refusals both halves (:122-124, :228-231); "on re-genesis" replaces "after resume" (:339-340);
  `reflect`/`reify` no longer conflated (:207-212); `mdformat` clean.
- **Nits — all closed:** "r18-fixed" gone — now "17 system dims … plus the `home`/`d.dims`
  genesis pair … — nineteen cells all told" (:313-315; verified `initGenesis` :29-70 mints
  exactly 17 `mintNamedDimension` calls at :50-63 and :67-69, `home` :35, `d.dims` :44);
  `1.`-numbered sub-steps explained by parenthetical (:372-373); external references now cited
  (commit `0bd47bc1` = the `cell_path` link fix, verified; `vagent-agent-language.md:1670`
  twice); clone names uniform (`cloneExpr`/`cloneDecl`/`cloneProgram`, :74);
  `SourceLocation`/ordinal qualifier restored (:109-110).

## New findings (only what is new or still wrong)

### MINOR

- **m-1 — Phase-5 (c) over-scopes the bind-set install** (:334-335). The bullet reads "(c)
  `programRef` → program root bind — the phase-3 bind set's real arm: root, agent cell,
  apparatus dims, `validate` constants." In the spec's lettered sequence the apparatus *dims*
  resolve at **(b)** — "(b) `ensureDimension` find-or-mint … dims resolve from base/cache
  before any mint" (:2294-2296), which is also where the V10 sweep lives ("the adapter's V10
  `ensureDimension` sweep minting `std:agent`'s thirteen", :2303-2305). (c) is narrowly
  "`programRef → the program root` bind" (:2344-2345). After a round whose blocking defect was
  letter precision at this boundary, listing "apparatus dims" under (c) re-blurs the (b)/(c)
  division — an implementer could double-implement the dim bind or misassign it. Fix: "(c)
  `programRef` → program root bind, plus the bind set's non-dim members (agent cell, `validate`
  constants) — the apparatus dims' names already resolve through (b)'s `ensureDimension` arm."
- **m-2 — The `closed` flag's named provenance has no producer** (:189-190). "the
  `CompilerBinding` state-side marker bit is its provenance, `compiler.hpp:15-18`" — but
  `CompilerBinding` today is exactly `{CellRef cell; Type type;}` (:15-18, verified); no marker
  bit exists, and no work item mints one. Phase 2's stated carrier is different: "per-fragment
  arity/purity/`closed` tables" (:158-159). A reader checking the cite finds no bit, and the
  plan never says whether the flag rides the tables or a new binding field — the
  consumed-not-produced pattern of r1 M-4 at small scale. Fix: name the carrier in phase 2's
  tables item ("the `closed` flag lands in the per-fragment tables; `compileFragment` reads it
  into the descriptor") or add a `CompilerBinding` field to a named item.
- **m-3 — The filing enumeration silently narrows the spec's "bind set"** (:312-313). "plus the
  fixed list: **the phase-3 bind set above** (the apparatus dims and the `validate` cells)" —
  spec :3018 files "the bind set above", which per :1053-1060 is program root + agent cell +
  twelve dims + `validate` constants. The parenthetical drops root and agent cell without
  saying why — defensible if the intent is "only name-bearing members file" (filing is
  `d.dims`-rank names), but then the `validate` *cells* don't belong either, since they are
  bound cells, not names. As written the list is narrower than the spec's on one side and wider
  than the name-table semantics on the other. Fix: say "the bind set's name-bearing members —
  the apparatus dims — plus the bind itself at (c)" or enumerate all four and note which are
  filed as names vs. bound as cells.

### NIT

- **n-1** (:303): "`job.base` pattern" carries no cite; the spec cites
  `mesh_jobs.cpp:304-318` — worth carrying over (pattern verified real: `mesh_jobs.cpp:131`,
  `mesh_host.cpp:488`, `mesh_swarm.cpp:797`).
- **n-2** (:307): `store.cpp:1388-1405` covers only `addScroll`'s body; the cited pair's other
  halves — `bindPublishedLocalScroll` (:1418) and `setContentSource` (:1425) — sit just outside.
  Extend to `:1388-1425`.
- **n-3** (:322): "`d.op-label`/`or_parallel` the known members" — `or_parallel` is the
  subsystem, not a dim name; its minted dims are `d.or`/`d.branch`/`d.frontier`/`d.submission`
  (`or_parallel.cpp:181-184`), and `d.op-label` is `ensureDimension`-minted at
  `mesh_value.cpp:473`. The claim is the plan's own, not the spec's — accurate in substance,
  but a reader could parse `or_parallel` as a dim name.
- **n-4** (:3): "(v1.19 — converged at review round 18; v1.19 records the P15/P16 approvals)" —
  v1.19 named twice in one breath, and it was v1.18 that converged at r18; v1.19 is the
  approvals revision. Cosmetic: "(v1.19 — the r18-converged text plus recorded P15/P16
  approvals)".
- **n-5** (:75, :121, :219-220, :263-264): mdformat's wraps inside slash-separated lists leave
  odd spacing — "recipients/ deadline", "maxExpansionDepth/ maxSplices", "d.recipe/ d.refFor",
  "`learn_where`/ `learn_body`". CI-clean (mdformat produced them); cosmetic only.

## Consistency checks run clean

- **Phase map vs bodies:** every row matches its section title and content; deps linear
  0→…→7; risk column defensible (4–6 high).
- **Requires vs exits:** phase 3's exit is reachable under the stated kernel-simulable arm
  (`run`, resolvers, `reify` engine-level; import/member *checking* is checker-side); phase 5's
  exit is honest about the test-owned genesis driver; phases 6–7 marked fully adapter-gated and
  their exits demand nothing more.
- **Terminology:** `kernel-simulable`/`adapter-gated` defined once (:47-48) and applied
  uniformly; `bind set`, `emission contract`, name-record/ref-record arm split, `the pin`,
  `first-resolution`, the mint/file → fold → pin/inject → first-resolution formula used
  identically in the phase-5 goal (:297-300) and phase-6 preamble (:369-372). `run(c, [args])`
  is the spec's own spelling (list literal, not optionality — :2948).
- **New cites verified:** `vortex_vm.cpp:854-893` (Call/Return frame mechanism),
  `mesh_actor.cpp:473-477` (executor_ call inside `pumpOne`, :460), `:182`/`:188`/`:261`
  (resume signature/`!checkpoint.store`/`userPermascrollPtr`), `dimension_registry.cpp:194-217`
  (`getOrCreate`'s rebuilt fold), `arena_manifold.cpp:34-46` (`dimensions()` base∪`arenaDims_`),
  `:78-105` (`ensureDimension`/`arenaDims_` write at :89), `:289-290` (`d.stores`/`d.store-refs`
  in `attach()`), `store.cpp:123` (`Store::Store`), `publication.cpp:1129-1131`
  (setContentSource/addScroll/bindPublishedLocalScroll call pattern), `compiler.cpp:884-891`
  (`const Expr *`-keyed tables incl. `localReferences_` :885), `:160-181` (`binding()` render
  check → `externalReferences_` :181), `:108-110` (baseline check), `:500-502` ("construct is
  not supported"), `:769-772` (`dimConstant`→`findOrMintDimension`), `checker.cpp:2069`
  (`builtinTypes`), `checker.hpp:30` (`check` public API), `mesh_evaluator.cpp:735`
  (fresh-`"cell"`-marker `link`), `tests/xuzz/vagent_compiler_test.cpp:18-46` (`ScalarHarness`
  :22-53), `Makefile:643`/`:649`/`:688-690`/`:692`/`:698`/`:1147`/`:1348-1350` — all land.
  `xuzz_test` links `XUDU_CORE_OBJS` = whole `apps/common/xanadu` tree (Makefile:999-1000);
  `VAGENT_RUNTIME_CORE_OBJS` filters `vagent/` out (:698); `test/vagent-*` targets exist.
- **Post-repartition contradictions:** none found. The interim compile-refusal for surviving
  `Code` nodes (phase 2, :163-165) is correctly superseded by phase 3's `compileFragment`
  lowering; phase-4's "exercised at genesis (b) — phase 5" (:260) honestly marks the P15 pass's
  first exercise point; phase 6's (iii) conditional owe matches §10.2.

## Top-3

1. **m-1** — phase-5 (c) lists "apparatus dims" under the `programRef`→root step when the spec
   resolves dims at (b)'s `ensureDimension` sweep; after a round defined by letter precision at
   this exact boundary, fix the parenthetical.
2. **m-2** — the descriptor's `closed` flag is given a `CompilerBinding` "marker bit"
   provenance that doesn't exist at `compiler.hpp:15-18` and that no work item produces;
   reconcile with phase 2's per-fragment tables.
3. **m-3** — the filing list's "(the apparatus dims and the `validate` cells)" silently narrows
   the spec's "bind set above"; say which bind-set members file as names vs. bind as cells.
