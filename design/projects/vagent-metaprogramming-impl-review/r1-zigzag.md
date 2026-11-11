# R1 (impl-plan) — ZigZag/promotion-semantics critique

Lane: cell-graph topology — dimension discipline, filing/promotion ordering, link
eviction, genesis sequencing. Target: `design/vagent-metaprogramming-implementation.md`
v1. Reference: `design/vagent-metaprogramming.md` v1.19 (mechanism text identical to
v1.18; the only delta is the P15/P16 approval labels).

Scope: promotion/checkpoint sequencing only — genesis ordering, dim filing,
marking-vs-ceremony split, inventory/eviction hazards. Verified every plan cite I
relied on against the tree (`vortex_core.cpp:30-69`, `worker_context.hpp:52-54`,
`vortex_vm.cpp:98-100`, `arena_manifold.cpp:289-290`, `Makefile:688-690`) — all land.

## Findings

### B1 — BLOCKING — plan:144-146 — the genesis (d)/(e) order is stated inverted, and the letters are corrupted too

**Claims:** "Genesis (e)/(f) split: (e) name-record rebind for registered heads *and*
replay-installed copies; (f) `d.patch` replay after rebind — patch-installed symbols
resolve to post-replacement bindings."

**Spec:** §6.6's fixed genesis order is (a) lineage bind → (b) `ensureDimension` +
`setOpcodeKind` → (c) `programRef` bind → **(d) `d.patch` replay in ordinal order**
(:2346) → **(e) `+d.refs` rehydration/rebind sweep, *last genesis pass*** (:2381) →
(f) turns. The spec spends a paragraph on *why* the sweep is last (:2435-2439): "a
name-record's rebind must see **post-installation** symbols — under the naive
pre-replay position a `learn_method`/`learn_plan` install-or-repoint replayed at (d)
was invisible to it … the misbind/drop class the reorder removes." The reorder is the
v1.7 R7-B fix (:3552-3555); the plan's stated order is the pre-v1.7 arrangement that
was deliberately reversed. Two extra inconsistencies: the plan's own coverage phrase
("replay-installed copies") presumes copies that cannot exist if the sweep runs
before replay, and spec's (f) is *turns* — replay is (d), so the letter citation is
wrong as well as the sequence.

**Fix:** restore spec order — (d) `d.patch` replay, then the (e) sweep over
registered heads *plus the install set (d) hands over* — the hand-off is itself
spec'd (:2373-2380) and worth naming since the copies hang off program-side anchors
no head-only traversal reaches — then (f) turns. Phase-5 exit should add a rebind-
against-patch-installed-symbol case (the §9.3 shape: a learned clause calling a
learned method), the exact misbind the reorder exists to prevent.

### M1 — MAJOR — plan:173-176 — the dim-filing vocabulary drops the bind set, and no phase schedules binding it at all

**Claims:** "name-table enumeration + explicit vocabulary — 19 system dims
(`vortex_core.cpp:50-69`), 13 V10 `std:agent` dims with sweep-before-filing
ordering, `d.store-refs`, `dimConstant` mints."

**Spec:** §10.1's fixed list has **five** members (:3016-3027): "**the bind set
above**, the system dims …, the thirteen V10 …, `attach()`'s `d.store-refs`, and
every `dimConstant`/`findOrMintDimension` mint". The dropped first member is the
adapter bind set — `d.code`/`d.patch`/`d.weight`/`d.synapse`/`d.entry`/`d.patch-head`/
`d.validate`/`d.refs`/`d.ref`/`d.recipe`/`d.refFor`/`d.mesh.relocation` plus the
validate ceiling, arity and impure-kind cells — i.e. **this document's own apparatus
dims**. And the plan never schedules the bind itself: §6.1's "adapter-supplied
bindings (V10's mechanism, minted with no marks outstanding): the program root cell,
the agent's own cell, the data-minted dimensions …, and the `validate` constants"
(:1053-1060, §1's "owed (adapter duty)" row) appears nowhere — not in step 2's
ceremony, not in step 3's vocabulary, not in phase 5 (whose `d.patch` rank and
agent-cell `d.vars` serial depend on it). The name-table arm only catches them if
they are `ensureDimension`-minted *before* filing — an ordering the plan also never
states for the apparatus set. The failure is the one r18 fixed for `+d.belief`/
`+d.intention`: unfiled `d.patch` → genesis `ensureDimension` mints a twin → the
replay walks the twin's empty rank → every learned patch silently dropped. The
phase-6 orphan-twin exit only nets this if it enumerates the apparatus names, and
the plan scopes that test to "the V10 test the language spec already describes".

**Fix:** add the adapter bind step — program root, agent cell, the apparatus dims,
the validate cells, V10 mechanism on the no-marks genesis path — ordered before
filing (or performed by it, the same license the V10 sweep gets), and list the bind
set explicitly in step 3's vocabulary beside the system/V10/`d.store-refs`/
`dimConstant` entries. Extend the orphan-twin exit to the apparatus dims.

### M2 — MAJOR — plan:168-176 — the file-before-pin sub-order never says where the pin lands; the emit-side filing is unscheduled

**Claims:** sub-steps 2 (lineage-store ceremony) → 3 (dim filing), with
"file-before-pin" named inside step 3.

**Spec:** the hazard is concrete (:3031-3039): `getOrCreate` move-assigns its
`Manifold&`, so the rebuilt fold the filing returns *is* the injected base — "a
filing after the pin lands outside the sealed fold — the base image was folded
before the rank append, so the pinned base carries neither the filed cells nor
their `d.dims` edges — and the arena mints twins regardless". The pin is the
WorkerContext two-arg construction (base-injection, step 1's seam). The plan's
sub-order lands the full store ceremony in step 2 — the natural implementation
wires the spawn path there to test the seeding, pinning the base — and files in
step 3, i.e. *after* the pin. The invariant is named but the pin itself is placed
nowhere. Separately, "file-before-first-resolution" is the *emit-side* pin: "the
artifact's own image emission files the same vocabulary the same way before ITS
lineage's first resolution — before the compile arena's `initGenesis`
(`vortex_core.cpp:14,:29-70`), every source-named `dimConstant` name included"
(:3040-3046; also :561-567). No phase owns that work — it is a change at artifact
image emission, not in the runtime ceremony.

**Fix:** merge steps 2–3 into one spawn-seed ("seed → file → pin") or state
explicitly that the pin lands only after step 3's filing returns the rebuilt fold.
Name the emit-side filing and give it a phase — phase 3's emission scope or an
explicit artifact-emission step — so `int64(DimRef)` operands and dim-keyed edges
seal naming persistent cells.

### M3 — MAJOR — genesis (a)/(b)'s recorded inputs are consumed but never produced

**Claims:** phase 4's P15 bullet re-registers "over the recorded instruction
enumeration" (:121-122); phase 6's comparand asserts the `1..E_ops` band (:168-172).

**Spec:** both inputs live on the adapter's `programRef` bind record, recorded once
at load and never re-derived (:3057-3058, :2222-2229, :2322-2327 — the enumeration
as `produces` `MicroversionId`s or canonical post-fold positions, `E_ops` the image
prefix length in ops). Neither the recording site (emission time — the artifact
writes its instruction enumeration when it seals) nor the bind record itself is
scheduled anywhere; step 2 names the comparand but not the `E_ops`/enumeration it
needs, and the `programRef` bind (genesis (c)) is absent along with the rest of the
bind clause (M1).

**Fix:** name the `programRef` bind record — program-root bind + recorded image
extent + instruction enumeration — as a phase-6 work item, and state where the
recording is written (artifact emission, riding the same record `E_ops` rides).

### M4 — MAJOR — no genesis hook: nothing invokes (b)–(f)

**Claims:** phase 5 delivers "verb-replayed dedupe at genesis" and its exit reruns
genesis "in a test harness"; phase 6 delivers the lineage/P16 machinery.

**Spec:** §1's own row prices the integration separately — durable mutation is
"`learn_*` verbs + `d.patch` rank + genesis re-application — owed (`std:meta` +
`std:agent` genesis hook)" (:91). The plan implements the replay/rebind *logic* but
never names the invocation site: the adapter genesis path that runs (b)–(f) at
spawn and at resume, before turns. A test-only driver can fake it for phase 5's
exit; production durability needs the real hook or nothing replays.

**Fix:** name the hook — which adapter function invokes `d.patch` replay + the
(e)-sweep on the spawn and resume paths — and put it in phase 5 (harness-grade) or
phase 6 (wired to the real genesis path).

### M5 — MAJOR — `VAGENT_FRONTEND_SRCS` wiring covers 3 of the 5 additions; phases 1–2 are link-fatal

**Claims:** phase 0: "Makefile: add all three to `VAGENT_FRONTEND_SRCS` (the list is
explicit — the `cell_path.cpp` omission bug is precedent for forgetting)" (:43-44).

**Spec/tree:** §10.1 joins **five** files to that list (:3061-3063):
`expand.cpp`, `resolve.cpp`, `ast_clone.cpp`, `code_path.cpp`, `frontend.cpp`. The
list is explicit (`Makefile:688-690`: `token.cpp lexer.cpp parser.cpp checker.cpp
cell_path.cpp` + the vql/scanner tail). Phase 0 wires `resolve`/`ast_clone`/
`frontend`; phase 1 adds `code_path.{hpp,cpp}` and phase 2 adds `expand.{hpp,cpp}`
with no Makefile line — so `code_path`'s symbols and the `expand` stage `frontend`
sequences never link. The plan cites the `cell_path` precedent and then repeats it.

**Fix:** add "Makefile: `code_path.cpp` → `VAGENT_FRONTEND_SRCS`" to phase 1 and
"`expand.cpp` → `VAGENT_FRONTEND_SRCS`" to phase 2.

### Minor findings

- **m1 — plan:143** — "`learn_method` replace-then-unlink ordering" reads inverted
  against the spec's stated **unlink-first**: "unlink any existing `d.vars` symbol
  of the same name first — name walks are first-match … so appending would shadow
  forever" (:1453-1455), and replay "rides `graft_method`'s own verb" (:2363-2364).
  If "replace-then-unlink" means install-then-remove it is wrong; rephrase to the
  spec's wording.
- **m2 — plan:177-181** — the marker rule drops per-ref-cell freshness: a shared
  `"cell"` marker is impossible — `link` gives each cell one `-d` neighbour and a
  second ref's edge would evict the first (:1122-1124, :2142-2144) — the natural
  "one constant marker cell" implementation self-evicts. One clause fixes it.
- **m3 — plan:144-153** — omits the replay label re-mint: the installed copy's
  `patch:<n>` re-mints verbatim off the record, never re-links the record's own
  label cell, whose single `-d.name` occupancy would evict and drop the label at
  the **second** genesis (:2349-2357). The phase-5 exit ("learn→re-genesis→learn
  cycles") should require ≥2 re-geneses or it cannot catch a re-linking
  implementation.
- **m4 — plan:172** — "scroll-identity-aware via `scrollKey`" drops the rest of
  `globalKeyOf`'s arm set (:2235-2243): `breakMarkerScroll` compares by
  `breakMarkerKey`, and a disengaged id that is neither `localScroll` nor an
  out-of-`externals` id fails the compare outright.
- **m5 — plan:121-122** — "post-resume re-registration" misses the spawn arm:
  genesis (b)'s `setOpcodeKind` pass "runs at both" the spawn-time image bind and
  resume (:2335-2337) — `opcodeMap_` is equally empty at spawn, so without it the
  folded image's op cells are surgically writable from birth.
- **m6 — plan:94-96 vs 177-181** — the marking/ceremony split is honest in kind
  (spec step 1 puts marking "at emission", §10.1 puts the `+d.refs` mark rank in
  `compileFragment`), but phase 6 re-listing "emission marking (`+d.refs`
  inventory …)" reads as if record emission were phase-6 work — while phase-5's
  name-record rebind and the `learn` closedness re-verification already consume
  those records. Fix: phase 6's bullet = "the adapter ceremony over the phase-3
  marks"; if the `d.mesh.relocation` edges deliberately defer, say the records
  stay in phase 3 and the marker emission extends to `reify`'s regenerated
  records.
- **m7 — plan:73-82** — phase 2 omits `staticFuel` in `ScalarCompilerLimits`
  (`compiler.hpp:23-26`, spec :3070-3071); only the two `CheckLimits` budgets are
  scheduled (phase 1).
- **m8 — plan:174** — "19 system dims (`vortex_core.cpp:50-69`)" — the nineteen
  include the `home`/`d.dims` genesis pair minted at `:35`/`:44` (verified in
  tree), outside the cited range; the spec's own phrasing carries both cites
  (:120-122).
- **m9 — plan:177-181** — the P16 bullet drops the window discipline that defines
  the flag: *no turn of either kind runs between the flagged turn's end and
  `publishValue`*, issued synchronously from the same Executor callback
  (:2171-2179). `pending(pid)==1` + P4 wake are named; the no-interleave property
  is the third member.
- **m10 — plan:173-176** — the coverage qualifier "names whose edges can fold"
  and the honest residual (an unfiled session-minted dim accretes one orphan per
  checkpoint, :2397-2402; `d.op-label`/`or_parallel` are the known members) are
  unmentioned — one clause keeps the orphan-twin test's scope honest: filed names
  only.

### Nits

- **n1 — plan:3** — cites "v1.18"; the reference is v1.19. The delta is approval
  labels only, but bump the cite for hygiene.
- **n2 — plan:207-208** — the P17 gloss ("deferred-not-registered; the containment
  discipline keeps it unnecessary") matches spec :3112-3115 verbatim; could name
  the drain idiom + no-marks minting so the claim is self-contained.

## Verified accurate (the plan inherits v1.19 correctly here)

- Two-source filed-set enumeration (name table + fixed list) — the r18 R18-A
  shape — correctly transposed (:173-176 vs :115-130, :3011-3027).
- V10 sweep-before-filing ordering (:175 vs :127-129, :3021-3023) — the plan picks
  the sweep arm of the spec's "or the filing performs it" license; fine.
- Both ordering pins named (file-before-pin, file-before-first-resolution) — the
  *names* are right; only the attribution/ownership is missing (M2).
- Phase-5 (e)-sweep coverage "registered heads and replay-installed copies" — the
  v1.8 widening (:3608-3609) correctly carried.
- One-rank `+d.refs` inventory covering `+d.recipe` fields (:177-178) — the R16-A
  rule, correctly carried.
- `call` vs `run` writeback split — `int64(entryOp)` vs `int64(head)` (:180-181 vs
  :2057-2064) — correct.
- `pending(pid)==1` + P4's approved wake inside the P16 ceremony (:179-180) —
  correct; all four §10.2 store seams present in step 1, P4's wake in step 4 —
  the owed-surface register is fully mapped.
- `patch:<n>` off the agent-cell `d.vars` counter, never rank positions, no
  recycling after `unlearn`, user alias separate (:139-141 vs :1855-1880,
  :1952-1954) — correct.
- Phase-5 scoping of the sweep to the name-record arm alone is coherent: the
  `*handleTarget` arm is a no-op without promote since every pass kind-guards
  (:2208-2209) — the phase 5/6 split of (e) is *split* correctly even though the
  (d)/(e) order is inverted (B1).
- All plan cites I needed spot-check correctly: `worker_context.hpp:52-54`
  (two-arg ctor), `vortex_vm.cpp:98-100` (`setOpcodeKind`),
  `arena_manifold.cpp:289-290` (`d.store-refs`), `vortex_core.cpp:30-69` (genesis
  block + dim mints), `Makefile:688` (explicit frontend list).

## Counts

- BLOCKING: 1 — MAJOR: 5 — MINOR: 10 — NIT: 2

Top issues: (1) the phase-5 genesis bullet states rebind-before-replay — the exact
pre-v1.7 arrangement the spec's R7-B reorder reversed, and self-contradictory with
its own "replay-installed copies" coverage; (2) the dim-filing vocabulary omits the
bind set (this spec's own `d.code`/`d.patch`/`d.refs`/… dims) and no phase schedules
binding or minting them — the r18 orphan-twin failure reintroduced on the document's
own machinery; (3) the file-before-pin sub-order never places the pin and the
emit-side filing has no owner.

VERDICT: NOT CONVERGED
