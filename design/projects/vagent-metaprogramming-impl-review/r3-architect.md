# r3 — system-architecture review (phase ordering and deliverability)

**Verdict:** CONVERGED — implementable end-to-end; three minors and a nit set worth a
final-pass cleanup.

**Counts:** 0 BLOCKING / 0 MAJOR / 3 MINOR / 9 NIT

Lens unchanged. Plan under review: `design/vagent-metaprogramming-implementation.md` v3.
Spec refs `design/vagent-metaprogramming.md` v1.19; `vagent-agent-language.md` for
V10/§5.8/P4.

## Directed verification — R2-A (dim-record third arm): **CLOSED**

Landed correctly at both sites, in the right phase, with the right ordering:

- **(e), plan :418-430** — the bullet is retitled "last pass, **three record arms**" and
  delivers all three with correct dispositions: (1) the name-record role-split rebind
  (`call`-role `int64(entryOp)` vs `run`-role `int64(head)`) "over registered heads *and*
  replay-installed copies' regenerated records" — spec :2381-2383 verbatim; (2) the
  **dim-record arm** — "`+d.name`-bearing `+d.recipe` records (ref-capture dim records and
  `dim`-field members alike) write `Int64(DimRef)` re-resolved through `ensureDimension`'s
  name arm into the `+d.refFor`-paired slot or the member cell itself — … *never* the
  promoted twin `*handleTarget` names" — spec :2394-2402 verified verbatim, including the
  ref-arm exemption (`+d.name`-bearing records exempt from the `*handleTarget` arm,
  spec :2391-2393) handled by arm separation; (3) the ref-record arm stays phase 6 with
  P16. Right arm: `Int64(DimRef)`, not the promoted twin; right ordering: "It runs after
  (d) precisely so name-record rebinds see post-installation symbols (the R7-B ordering,
  spec :2435-2439)" — the spec's ordering clause verified verbatim; right phase: in phase
  5 off the filed rank, "not a no-op pre-P16 — it runs at every genesis" — matching the
  disposition's "unlike the ref arm it is *not* a no-op pre-P16".
- **(d), plan :413-416** — the donor-slot derivation carries the same exception: "the
  `+d.name`-bearing dim record excepted, where the record's `+d.name` *is* the endpoint
  source and the slot takes `Int64` of the resolved `DimRef`, not the donor's promoted
  twin (spec :2364-2372)" — spec :2365-2372 verified verbatim.
- **Exit, plan :445-446** — "a `dim`-field recipe record re-resolved to `Int64(DimRef)`
  off the filed name post-fold" now asserts the arm. The r2 objection — spec'd arm
  slipping between two enumerated arms, unasserted by any exit — is fully answered.

## Directed verification — R2-B (re-genesis loop + Requires): **CLOSED**

- **Driver bullet, plan :376-385** — "The harness's **re-genesis loop** is named
  machinery": session mutations append no ops, so each cycle runs
  `promote(Store&, parent, arena, root)` (`arena_manifold.hpp:845-847` — signature
  verified at :844-846, `(Store&, MicroversionId, const ArenaManifold&, CellRef,
  PromotionBudget)`, into `arena.base()->store()`) → `Store::rebuildManifold` over the new
  head → re-attach through a fresh `WorkerContext(base, reader)` — the two-arg ctor taking
  `const Manifold *` + `const SpanReader *` verified `worker_context.hpp:52-54`, and the
  freshness rationale is code-true (the ctor owns `arena/core/vm/stdlib`, so `opcodeMap_`
  is per-context) → re-run (a)–(e). The no-`sealLocalSpool` note and the
  `adoptOpRecords` (`store.cpp:3836-3940`, verified) band alternative are correct.
- **Requires, plan :436-440** — now lists `promote`, `rebuildManifold` and a fresh
  `WorkerContext` per cycle, plus the `overlay.cpp:46-82` precedent — verified:
  `declareOverlayTarget` runs exactly the mint→`rebuildManifold`→re-pin loop
  (`makeDimension`→rebuild at :51-53, `makeCell`→rebuild at :68-76, `setLink`→rebuild at
  :80-82) with no actor. Kernel-simulable claim honest.
- **Exit, plan :442-444** — "≥2 re-geneses" matches spec :2351-2354 (a re-link drops
  `patch:<n>` only at the second fold).

## Minor bundle — applied, spot-verified

Every bundle item landed: replay-does-not-re-record with the `reify:<src>:<serial>`
shared-counter carve-out (:407-409; spec :2346-2350 verified); verbatim `patch:<n>`
re-mint with the second-genesis eviction rationale (:410-413; :2351-2357 verified);
`CompilerBinding` marker bit scheduled as *new work* with `{cell, type}` cited
(`compiler.hpp:15-18`, spec :2958-2959 — both verified); filing narrowed to
names-vs-cells (:257-259, :362-364); (b) split into `ensureDimension`-names and
`setOpcodeKind`-enumeration arms, both at every fresh-context bind (:395-399;
spec :2294-2343 verified); (c) scoped to the bind pass (:400-402; :2344-2345 verified);
cite fixes — span-source pair at `store.cpp:1418`/`:1425` (verified:
`bindPublishedLocalScroll` :1418-1423, `setContentSource` :1425, `addScroll` :1388-1405 =
the "three bodies"), `d.op-label` `mesh_value.cpp:473` (verified), `or_parallel`'s four
`:181-184` (verified verbatim), `zigzag_test` in `make test` :1348-1351 (verified);
`fragment.{hpp,cpp}` under `vagent/` with the filter-out rationale (:221-223; §10.1's
tree at spec :2920-2944 verified — the spec itself marks it "kernel-side, picked up by
the existing source glob"); `learn_method`'s name "never an alias" (:322-325;
spec :1850-1853 verified); `run`/`reify` purity-vs-functional-split wording (:229-230);
phase-6's ref arm moved inside step 3 (:478-480, my m-4); coverage floor names
frame-bound classes before the refusal rules (:264-268; spec :846-854 verified —
`def`/plan/handler parameters *and* `let` locals); env-walk fallback resolved
(:170-173 — my n-5's lower-risk pick adopted); skeleton-refusal scope pinned
program-wide/body-recursive (:115-117, meta F1); `closed` off outside-reference
classification (:174-177, F3); descriptor schema beside `closed` (:210-214, F4); all
three m-5 tightenings ((iv) tree-local-testable :466-468, mailbox-empty-at-turn-end with
the self-send caveat :482-483 — spec :2163-2169 verified the proxy framing —
emit-side filing marked owed-substrate :369-371); `test_of`/`body_of` memo record chain
priced (:238-242; spec :311-331 + member rows :1157-1158 verified); `reify`'s serial
counter named in phase 3 (:246-248; :2505-2509 verified); resolver-test honesty
parenthetical (:283-284); step (i) `Manifold*`-not-`Store` (:461-463); `#Call` wording
(:260-263); clause-edge re-point (:406); prelude home `vagent/prelude/` (:233-235);
conditional-last (iii) (:469-471); Emitter's tables (:162-164). `vortex_core.cpp:14`
turns out **correct** — the ctor's `initGenesis()` call sits at :14 (verified) and the
spec itself cites `:14,:29-70` at :2301; my r2 n-1 (`:8-9`) was the error.

Spec-side spot-verified beyond the r2 set, all clean: :2228-2232 (pairwise comparand),
:2235-2243 (`globalKeyOf` arm set), :2249-2258 (permutation/refusal region),
:2285-2293 (prefix-less fail-closed), :2294-2343 ((b) both arms + per-context map),
:2344-2345 ((c)), :2346-2380 ((d) replay + install-set hand-off), :2381-2439 ((e) all
three arms + both ordering reasons), :914-920 (descriptor), :1376-1384 (head-valued slot,
result/produced), :846-895 (frame-bound classes + capture-liveness), :1141-1144
(head-vs-op discriminator), :2500-2504 (reserved prefixes at module declaration),
:2958-2959 (marker bit), :2920-2964 (file tree), :3057-3072 (record enumeration +
`staticFuel`/`CheckLimits`), :3077-3105 (seams (i)–(v) incl. (iii)-conditional and the
`documentId` arm), :3110 (P16 row: marker inventory, role-split rebind, exemption),
:2163-2184 (flag windows + no-interleave), :1850-1864 (`patch:<n>` mint + record fields),
:1157-1177 (member rows incl. `learn_*` "conditional on P16"), :91/:156 (same),
:2905-2907 (§9.1-only scalar subset). Code-side: `mesh_actor.cpp:182`/`:188`/`:261`
(resume's transported-store guard + `receiveValue` verify — matches spec's own cites),
`:473-477` (`executor_` callback inside `pumpOne`), `mesh_evaluator.cpp:725-735` (the
`link(cell, relocation, "cell")` marker precedent), `store.cpp:123` (`Store()` no-id
ctor), `mesh_jobs.cpp:304-318` (`job.base` — the spec cites the same lines), Makefile
:643/:649/:688-690/:692/:695/:698/:1147/:1348-1351, `checker.hpp:12-27`/`:29-30`,
`compiler.hpp:15-18`/`:23-26`, `checker.cpp:2069` (builtinTypes), `parser.cpp:848-855`
(the `cells`-only gate `code` joins).

---

## MINOR

### m-1 — The re-genesis loop's `promote` root is unspecified — and session mutations span two disjoint roots

**Plan :378-385; spec :2263-2267; `arena_manifold.hpp:837-846`.**

"each cycle promotes them into the fold — `promote(Store&, parent, arena, root)`" shows
one call with `root` left abstract. `promote` folds only the **reachable subgraph** of
its `root` argument (`arena_manifold.hpp:837-838` — "nothing … when the reachable
subgraph exceeds @p budget"). But the cells that must fold live under **two roots never
`+d`-reached from each other** (spec :2265-2266): the `d.patch` records, `d.code`
registrations and the `d.vars` serial counter hang off the **agent cell** (and must fold
— the counter is "a durable per-agent counter … not a per-incarnation one",
spec :2505-2509, and (d) replays out of `d.patch`), while installed copies and
`d.clause`-anchor mutations hang off the **program root**. A single `promote` covers one
side or the other, unless the arena keeps a session root linked to both (runtime-grown
cross-root `Link`s exist but are not construction-guaranteed, spec :2278). An
implementer reading the bullet literally writes one call and silently drops half the
session — caught loudly by the ≥2-re-geneses exit, but the two-root fact is exactly the
kind of spec subtlety this plan elsewhere prices. One clause fixes it: "promote once per
runtime root (program root and agent cell — never mutually reachable, spec :2265-2266),
or under a harness session root linked to both".

### m-2 — The (e) ref arm's no-op rationale misstates the code: `OpHandle`-kind cells are minted today

**Plan :427-430 vs `store.cpp:827`/`:2239`/`:2336`/`:3360-3366`/`:3437-3443`/`:3497`/`:3547`/`:3626-3632`, `mesh_evaluator.cpp:733`, `view_manifold.cpp:68`, `arena_manifold.cpp:1947-1954`, `mesh_value.cpp:465-466`.**

"a no-op here because nothing mints `OpHandle`-kind cells pre-P16 (`writeValue` covers
Int64/Double/Bool/None only, `vortex_core.cpp:196-229`)". The `writeValue` parenthetical
is accurate (the `CellValue` variant is double/int64/bool/string; the string arm sets
kind `None`, :217-227 — verified). The headline is not: `Store::makeOpHandle` mints
`OpHandle`-kind cells in six call paths today, `setValueBits(..., ValueKind::OpHandle,
...)` re-kinds cells at `mesh_evaluator.cpp:733` — the *same function* the plan cites
four bullets later at `:735` as the marker precedent — `view_manifold.cpp:68`,
`ForeignOpHandle` mints at `mesh_value.cpp:465-466`/`store.cpp:846`, and `promote`
translates arena `OpHandle` cells into store op handles
(`arena_manifold.cpp:1947-1954`). The deferral itself is spec-correct; the true, and
sufficient, reason is narrower and one spec-line away: no *fragment `+d.refs` ref slot*
is `OpHandle`-kind before the re-kind — emission writes `Int64`, and `reify`'s
`New`/`Link`/`Value` palette cannot mint handle-kind cells, so the arm's kind-guard finds
nothing (spec :2430-2432). The narrower phrasing also matters to phase 6's reader: the
arm must stay kind-guarded precisely because post-replay the two kinds coexist —
palette-minted copy slots arrive `Int64` while checkpoint-re-kinded donor slots arrive
`OpHandle` (spec :2417-2432). Replace the clause with the palette/kind-guard reason.

### m-3 — (e)'s name-record arm names the rebind but not the resolver that performs it

**Plan :419-420; spec :3110.**

Every other arm in the driver names its machinery — (b) names `ensureDimension` and
`setOpcodeKind`, the dim arm names `ensureDimension`'s name arm — but the name-record arm
states only the write targets (`call`-role `int64(entryOp)` vs `run`-role
`int64(head)`). The spec's arm is a resolver walk: `call`-role resolves the qualified
`module:Owner.name` through `symbol_named`'s module-`d.vars` walk → `+d.values` → the
op-or-head hop → `writeValue(slot, int64(entryOp))`; `run`-role resolves its
`src:`/`reify:`/`#label` through the program-image label→head lookup →
`writeValue(slot, int64(head))` (spec :3110). Both resolvers ship in phase 3
(`symbol_named`, `code_named`), so nothing is missing as machinery — but a bullet that
enumerates the other arms' routines should name these two. One parenthetical.

---

## NIT

- **n-1 — "~10 `check(*program)` call sites" is ~25** (:125-126, :171-172). Every
  `check(` call in `tests/xuzz/vagent_checker_test.cpp` — 25 of them — bypasses
  resolve/expand; the substance (public API, env-walk fallback) is right, the count is
  low by ~2.5×.
- **n-2 — durable-semantics "the user alias" retains the m-2 misnomer** (:433). The (d)
  bullet's corrected phrasing doesn't reach here; for `learn_method` the recorded name
  *is* the lookup name, never an alias. "the recorded target name (user alias or
  `learn_method`'s functional name) separate and un-minted".
- **n-3 — `closed` summary drops the third counting class** (:175-177; spec :158).
  "cap$N captures and name-resolvable program refs both count *toward* `closed`" omits
  the `cells"…"`-path `int64` ref-box state-side arm the glossary lists beside them.
  "state-side refs (`cap$N` captures, ref-box `int64` slots) and name-resolvable program
  refs".
- **n-4 — `arena_manifold.hpp:845-847` → :844-846** (:380). The declaration starts one
  line earlier.
- **n-5 — `Makefile:698-699` overshoots** (:286, :530). The `vagent`-object filter-out is
  :698 alone (comment :697; :699 is `SPIKE_SRCS`). The range contains the right line but
  reads as if two lines carry it.
- **n-6 — (a)'s "missing prefix" arm lands outside its cite** (:393-394). Permuted-band
  refusal is spec :2249-2258; the prefix-less fail-closed arm is :2290-2292. Extend the
  cite.
- **n-7 — "a test-file edit beside `main.cpp`'s"** (:126). `main.cpp` is the driver, not
  a test file — "or a test-file edit, beside the `main.cpp` rewiring".
- **n-8 — `vagent/prelude/` vs the nearer precedent** (:233-235). The named home closes
  r2's n-4, but `vlace/prelude.hpp` — a sibling file beside its frontend — is the in-tree
  precedent; a `prelude/` subdir is fine, worth acknowledging either way.
- **n-9 — phase-6 exit stops one resume short** (:492-493). "learn → checkpoint →
  resume → learn → checkpoint" never re-resumes to assert the *post-resume* learn
  survived a second fold. A trailing `→ resume` closes the round-trip.

## Cross-cutting note for the disposition

The v3 revision is a faithful application of the r2 rulings — both directed fixes landed
at both required sites with correct spec mechanics, and the entire minor bundle applied.
I re-verified the new cites against code and spec; the only stale number is the "~10"
test count, and my own r2 `vortex_core.cpp:14` nit turned out to be the error (the spec
itself uses that line for the ctor's call). Phase ordering remains sound: no used-before-
built, no exit outrunning its Requires. The three minors are clause-level — one
unspecified argument inside the new R2-B machinery (promote's root, load-bearing because
the two runtime roots are structurally disjoint), one code-false rationale inside the new
R2-A text (whose deferral conclusion is nonetheless right), one missing resolver name.
None changes a work item; the plan is implementable as written. m-1 and m-2 are the two I
would apply before calling it done.
