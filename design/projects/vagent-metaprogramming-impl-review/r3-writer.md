# r3-writer — document-coherence critique of `design/vagent-metaprogramming-implementation.md` (v3)

Verdict: **CONVERGED**. Counts: **0 BLOCKING / 0 MAJOR / 1 MINOR / 3 NIT**.

Every r2 item verified landed — both majors (R2-A, R2-B) and the full minor bundle, checked
clause-by-clause against v3 and re-verified against spec v1.19 and the tree. Residual is one
wording under-specification exposed by the new (e) dim arm, plus three cosmetic items, none
load-bearing. `mdl -i` clean (exit 0); `mdformat --check --wrap 100` clean (exit 0). Every
`file:line` cite in the ~20 new clauses spot-checked clean.

## Per-finding verification (r2 → v3)

### My three minors — all CLOSED

- **m-1 (b)/(c) division — CLOSED.** Phase-5 (b) now reads "**two arms, not one scope**":
  `ensureDimension` find-or-mint over dim *names* (filed → canonical cell, unfiled → re-mint —
  spec :2294-2299, :2401-2402 verified; "the V10 sweep included") **and** `setOpcodeKind`
  re-registration over the recorded instruction enumeration — both at every fresh-context image
  bind (:395-399). (c) is now "`programRef` → program root bind (spec :2344-2345), **plus the
  bind set's non-dim members — agent cell, `validate` constants; the apparatus dims' names
  already resolved through (b)'s `ensureDimension` arm**" (:400-402) — my suggested wording near
  verbatim. The (b)/(c) boundary is precise again; consistent with spec (b) grouping both arms
  under one letter (:2294-2299) and (c) naming only the root hop (:2344-2345).
- **m-2 `CompilerBinding` marker bit — CLOSED.** The bit is now a named work item in phase 2's
  `compiler.cpp` bullet: "the `CompilerBinding` **state-side marker bit** §10.1 adds for
  `closed`'s provenance (spec :2958-2959 — the struct is `{cell, type}` today,
  `compiler.hpp:15-18`; the bit is new work here, not an existing field)" (:189-191 — spec
  :2958-2959 verified verbatim, struct verified at `vagent/compiler.hpp:15-18`). The flag's
  carrier is the per-fragment tables: "`closed` is computed here … The flag lands in these
  tables — `compileFragment` reads it into the descriptor in phase 3; `learn` re-verifies it in
  phase 4" (:174-178), and phase 3 consumes it consistently (:213-214). Consumed-not-produced
  resolved.
- **m-3 filing name-vs-cell — CLOSED.** The filing bullet now states the rule: "filing is
  name-table entries, not cells: **the bind set's name-bearing members** — the twelve apparatus
  dims — file here, while its bound non-dim members (program root, agent cell, `validate`
  constants) bind at (c) and never file" (:361-363). Phase 3's bind-set bullet repeats the same
  split (:256-259). Correctly disambiguates the spec's "the bind set above" (:3016-3017, where
  filing is names-only). Consistent end to end.

### r2 majors — both CLOSED

- **R2-A (e)'s third arm — CLOSED.** (e) now reads "**last pass, three record arms**" (:418):
  name-record role-split rebind (:419-420, `int64(entryOp)`/`int64(head)` verified vs spec
  :2061-2063), the **dim-record arm** — "`+d.name`-bearing `+d.recipe` records (ref-capture dim
  records and `dim`-field members alike) write `Int64(DimRef)` re-resolved through
  `ensureDimension`'s name arm into the `+d.refFor`-paired slot or the member cell itself —
  a filed-cell rebind, *never* the promoted twin" (:420-424, verified verbatim against spec
  :2394-2402), with the right rationale ("stale compile-arena ids … *not* a no-op pre-P16",
  :424-425) and the ref arm deferred with an honest reason ("nothing mints `OpHandle`-kind
  cells pre-P16 — `writeValue` covers Int64/Double/Bool/None only, `vortex_core.cpp:196-229`",
  :427-430 — verified). (d)'s donor-slot derivation gained the same exception
  (:413-416, spec :2364-2372 verified). Phase-5's exit now tests the arm ("a `dim`-field recipe
  record re-resolved to `Int64(DimRef)` off the filed name post-fold", :445-446), and the
  schema's promised consumer exists.
- **R2-B re-genesis producer — CLOSED.** The harness's "**re-genesis loop** is named machinery"
  (:378-385): `promote(Store&, parent, arena, root)` (`arena_manifold.hpp:845-847` — signature
  verified) into `arena.base()->store()` → `Store::rebuildManifold` over the new head →
  re-attach through a fresh `WorkerContext(base, reader)` (`worker_context.hpp:52-54` — two-arg
  `const Manifold *` + `SpanReader` ctor verified) → re-run (a)–(e). Requires line lists the
  harness objects (:436-438) and `overlay.cpp:46-82`'s identical mint→rebuild→re-pin loop is
  cited — verified real. Exit demands "≥2 re-geneses" (:442-443).

### Minor bundle — all landed

(d) "replay does not re-record" with the `reify:<src>:<serial>`-still-mints clause
(:407-409, spec :2346-2350, :2504-2509 verified); `store.cpp:1388-1425` cite extended with the
pair located at :1418/:1425 (:355 — `bindPublishedLocalScroll` :1418, `setContentSource` :1425
verified); `d.op-label`/`or_parallel` code cites appended (`mesh_value.cpp:473`,
`or_parallel.cpp:181-184` — all four dim names verified); `fragment.{hpp,cpp}` placed under
`apps/common/xanadu/vagent/` per spec :2944 with the filter-out rationale (:221-223 — spec's
tree header verified, `COMMON_XANADU_SRCS` glob at Makefile:643 verified); `learn_method`'s
name "never an alias" (:322-325, spec :1851-1853 verified verbatim); `run`/`reify` purity
corrected — "the spec's *read group* proper is the pure top half" (:229-231, spec :1145-1147
verified); ref-record arm moved inside the P16 ceremony where its flag machinery exists
(:477-480 — the ordering defect gone); coverage floor + frame-bound classes (:264-268,
spec :847-854 verified); env-walk fallback for un-stamped `check()` input (:170-173 —
`checker.hpp:30` `check` public API verified); skeleton refusal scoped program-wide,
body-recursive (:115-117); `closed` off outside-reference classification (:174-177, spec :158
glossary verified); descriptor field schema (:210-213); `test_of`/`body_of` memo chain
(:238-242, spec :311-331 verified — fresh text cell, content-compared key, label-less → no
record, ~6-cell leak); wake tree-local-testable + `pending(pid)==1` proxy + emit-side filing
noted owed (:465-468, :482-483, :369-371); `WorkerContext` ctor takes `const Manifold *` not
`Store` (:461-462); "never a `Call`-opcode operand" wording (:261-262, spec :259-262 verified);
prelude skeleton home named `vagent/prelude/` (:233-234); (iii) listed last because conditional
(:469-471); `Emitter`'s tables (:162); `vortex_core.cpp:14` initGenesis cite (:370 — ctor at
:14, body :29-70 verified); Makefile cites corrected to :698-699 and :1348-1351 (:286, :530,
:532 — filter-out verified at :698, three test binaries at :1349-1351).

### Nits — all landed

`job.base` cite `mesh_jobs.cpp:304-318` carried (:352 — pattern verified); v1.19 header
phrasing fixed (:3-4); clone names uniform; the standing mdformat slash-wraps persist
(cosmetic, mdformat-produced — carried below as n-3).

## New findings (only what is new or still wrong)

### MINOR

- **m-1 — the emission contract's `+d.name` reservation is narrower than its new consumer**
  (:218 vs :421-422). Phase 3's "stated in full" schema reserves "`+d.name` on a `+d.recipe`
  member … to **the `dim` field's** dim-name channel". Phase 5's new (e) dim arm consumes
  "`+d.name`-bearing `+d.recipe` records (**ref-capture dim records and `dim`-field members
  alike**)". The spec puts `+d.name` on `+d.recipe` records in *both* places — "a dim referent
  name-recorded on its paired *ref-capture* record's `+d.name` — or, for a `dim` tuple field,
  on the field's own `+d.recipe` member's `+d.name`" (spec :1964-1967; :2393-2395 same). An
  implementer reading the contract strictly emits `+d.name` only on `dim`-field members,
  leaving (e)'s ref-capture half and `closed`'s (c)-arm with nothing to read. Borders nit —
  "dim-name channel" could be read generically — but the clause claims completeness and its
  new consumer enumerates two cases. Fix: extend the reservation — "`+d.name` on a `+d.recipe`
  record reserved to the dim-name channel — `dim`-field members and `+d.refFor`-paired
  ref-capture dim records alike" (mirroring (e)'s enumeration).

### NIT

- **n-1** (:418): "(e) **`+d.refs` rehydration — last pass, three record arms**" reads as if
  all three run here; the text self-corrects two lines later by deferring the ref arm to
  phase 6. Cosmetic — "the three-arm schema; two arms run here" would front the deferral.
- **n-2** (:382-383): "fresh because (b)'s `setOpcodeKind` writes `opcodeMap_` on the context's
  `VortexVM`" slightly inverts the reason — the fresh context is needed because re-attach
  points at the rebuilt fold (and the per-context map then needs re-fill, which phase 4's P15
  bullet states correctly). Cosmetic reasoning-slip, not a wrong instruction.
- **n-3** (carried): mdformat's slash-list wraps still produce "recipients/ deadline" (:76),
  "`consume`/ `terminate`" (:156), "`maxExpansionDepth`/ `maxSplices`" (:129), "`+d.refs`/
  `+d.recipe`" (:215), "`d.recipe`/ `d.refFor`" (:254), "`learn_where`/ `learn_body`" (:305).
  mdformat-produced, CI-clean; cosmetic only.

## Consistency checks run clean

- **Phase map vs bodies:** every row still matches its section; deps linear 0→…→7; phase-5's
  map line "(a)–(e) driver" matches the five-lettered body.
- **Post-addition contradictions:** none found besides m-1. The (e) dim arm's `ensureDimension`
  name-arm re-resolution coexists with phase 3's no-marks rule (:273-275) — genesis binds with
  no marks outstanding, the rule governs `reify`. The "(b) … the phase-4-registered P15
  machinery is what the second fills" (:398-399) ↔ phase-4's "exercised at genesis (b) —
  phase 5" (:302) cross-reference is consistent both ways. Phase-3's bind-set split
  (:256-259), filing bullet (:361-363) and (c) (:400-402) tell one name-vs-cell story. Phase-6
  step order (i),(ii),(iv),(v),(iii) is intentional and noted; the ref arm lives inside the P16
  step that produces its flags. `reify`'s `d.vars`-counter claim (:246-248) matches (d)'s
  replay note (:408-409) and the durable-semantics bullet (:431-434).
- **Requires vs exits:** phase-5's Requires now names `promote`/`rebuildManifold`/fresh
  `WorkerContext` and its exit covers every new clause — ≥2 re-geneses (re-mint detection),
  the §9.3 rebind, the `dim`-field `Int64(DimRef)` test, the (a)-arm permutation refusal, the
  orphan-twin test. Phase-3's exit names the coverage floor the body prices.
- **Terminology:** "non-mutation surface" / "read group (pure top half)" / "functional split"
  now agree with the spec's purity table (:1152-1174 — all 16 members verified); "bind set"
  used identically in phases 3/5/6; the mint/file → fold → pin/inject → first-resolution
  formula in phase 5's goal (:344-348) and phase 6's preamble (:453-457) is uniform.
- **New cites verified:** `arena_manifold.hpp:845-847` (`promote` signature),
  `worker_context.hpp:52-54` (ctor, `const Manifold *`), `overlay.cpp:46-82` (the loop),
  `store.cpp:3836-3940` (`adoptOpRecords`, verified to :3940), `store.cpp:1388-1425`
  (`addScroll` :1388, `bindPublishedLocalScroll` :1418, `setContentSource` :1425),
  `vortex_core.cpp:14`/`:29-70`/`:50-69`/`:35`/`:44` (`initGenesis` call + body + 17 dims +
  `home`/`d.dims`), `vortex_core.cpp:196-229` (`writeValue` kinds), `vortex_vm.cpp:98-100`
  (`setOpcodeKind`→`opcodeMap_`), `mesh_value.cpp:473` (`d.op-label`),
  `or_parallel.cpp:181-184` (four dims), `mesh_actor.cpp:182`/`:188`/`:261` (resume signature /
  `!checkpoint.store` / `userPermascrollPtr`), `mesh_actor.cpp:473-477` (`executor_` in
  `pumpOne`), `:659` (`pending`), `mesh_jobs.cpp:304-318` (`job.base`), `compiler.hpp:15-18`
  (`{cell, type}`), `Makefile:643`/`:649`/`:688-690`/`:692`/`:695`/`:698`/`:1147`/`:1348-1351`
  — all land.
- **Format gates:** `mdl -i` exit 0; `mdformat --check --wrap 100` exit 0.

## Top-3

1. **m-1** — phase-3's emission contract reserves `+d.name`-on-`+d.recipe` to "the `dim`
   field's dim-name channel" while the new (e) arm consumes ref-capture dim records too; the
   "stated in full" contract should enumerate both cases its consumers read.
2. **n-1** — "(e) … three record arms" fronts a count that includes the deferred ref arm;
   front the deferral instead.
3. **n-2** — the fresh-`WorkerContext` parenthetical names the map-write as the reason for
   freshness when re-attach onto the rebuilt fold is; phase 4 states it correctly.
