# r5 — codebase-completeness critic (inventory coverage and citation truth)

**Verdict: CONVERGED** — every v5 delta verified against code and spec: all eight disposition cite
fixes land exactly or within one line; R4-A (promote root = agent cell alone) landed with all three
supporting arguments spec-verified and no spanning-root remnant; both of my r4 nits applied
verbatim; the two mechanical claims added this round (unlink-vs-tombstone, first-fold orphan)
re-derive correctly from the arena/promote code. One residual MINOR: the `spec :2397-2402` cite
attached to "not a per-checkpoint accrual" contains the spec's contrary "per checkpoint" rate.

**Counts:** 0 BLOCKING / 0 MAJOR / 1 MINOR / 5 NIT

Scope of this pass: the full v4→v5 diff (six hunks) verified cite-by-cite; the majors-only sweep
re-derived the two new mechanical claims and confirmed no dependency inversion was introduced.

## R4-A landed — `promote`'s root is the agent cell alone

Phase 5's re-genesis bullet now reads (plan :441-450): "`root` is the **agent cell** alone (spec
:375, :634-635): program-side state is *never* a promote root — bare grafts must die with the
incarnation (spec :375-377, :1791-1799 — that boundary is what makes `learn` = graft + record a
distinction), graft-adds would carry without prune-removes, and replay-installed copies would
double under (d), whose alias re-match can't see a promoted `patch:<n>` label (spec :1322-1324)."
Each leg verified:

- spec :375 — "the checkpoint promotes what is reachable from the agent cell" (verbatim, §4.4).
- spec :376-378 — "program cells are therefore never *republished* — they ride the shared base
  lineage … — so a session graft is gone at `resume`" (the bare-graft boundary).
- spec :1791-1799 — "`State-side` is link-reachable *from the agent cell* … without transiting a
  program-side cell (the walk prunes at the program-side boundary)" and installed `learn` copies
  "sit behind program-side anchors … they die with the incarnation at `resume`".
- spec :1322-1324 — the verbatim alias re-match ("the match is *any* verbatim hit … a learned
  clause's `patch:<n>` label is unreachable — `patch:` is refused as a `name` argument") — supports
  "can't see a promoted `patch:<n>` label".
- "graft-adds would carry without prune-removes" — consistent with the promote class stated two
  sentences up (link-writes promote; unlinks never un-emit, `arena_manifold.cpp:2020-2053` verified
  r4, re-checked :2020-2053 unchanged).
- The spanning-root alternative is fully removed — grep finds no "session root", "runtime root",
  "linked to both", or "once per" residue. The only "runtime roots" left (:281) is bind-set
  vocabulary (resolver roots), correctly scoped.
- The image-seed carve-out is correctly wired: "The only program-rooted promote is the one-time
  image seed named below" → two lines later, "the initial image band comes from `promote` of the
  compile arena or `adoptOpRecords` (`store.cpp:3836-4018`)" — the named-below is real.
- `arena_manifold.hpp:833-836` — the `@param root` doc ("Only what is reachable from it along any
  dimension is promoted") at :833-836 exactly; the signature cite `arena_manifold.hpp:844-846`
  (:426) still lands separately. ✓

## The two new mechanical claims re-derive correctly

- **`unlearn` drop classes** (plan :437-441): mid-rank record drops are repoints (link-writes that
  promote); the tail drop is a true removal needing tombstone form. `arena_manifold.hpp:616-618` —
  `unlink(from,dim,dir) { return link(from, dim, dir, std::nullopt); }` — a removal write with no
  chain repair, exact. Mechanism: a repoint writes a new +d edge that evicts the old at fold; a tail
  unlink removes a base edge nothing can un-emit and (d) replay resurrects — consistent with the
  promote class and the (d) replay text. ✓
- **`d.vars` repoint precision** (plan :434-435): "a *fresh ephemeral* the keying record's
  `+d.values` edge repoints at (spec :1300)" — spec :1300 is "each scalar field its own `+d.vars →
  [key] +d.values → scalar` record" — the repoint target is the record's `+d.values` edge, exact.
  Better than v4's "the rank repoints at". ✓
- **First-fold orphan re-scope** (plan :411-418): "`makeDimension`'s store cell `B` while the
  session's edges named arena cell `A`, and `discover`→promote mints `A'` carrying them" —
  `store.cpp:1066` is `const auto minted = makeCell(parent, name, ctx)` inside `makeDimension`
  (the store-side `B` mint, exact); `arena_manifold.cpp:1781` is `discover(edge.dim, false)` —
  enqueues the dim cell into `order` without expanding (dims get minted, not walked past);
  `arena_manifold.cpp:2030-2042` is the link-write loop resolving `real.find(edge.dim)` →
  `store.setLink` — the `A'` twin carries the folded edges. "One orphan at its *first* fold" is
  mechanically airtight under the plan's own per-cycle filing + fresh `WorkerContext` per cycle
  (`worker_context.hpp:52-54` re-verified: `arena(baseManifold)` pins base at construction, moves
  deleted :57-60): a session mint lives in one arena's `arenaDims_`, folds to one `A'`, then the
  sweep files `B` and `dimensionNamed` resolves `B` thereafter — `getOrCreate`'s `held` check
  (`dimension_registry.cpp:198-200`) backs "idempotent by name". ✓ (Cite tension → m1 below.)

## The cite fixes — all land

| v5 cite | verdict |
| ------- | ------- |
| `arena_manifold.hpp:833-836` (was :837-846) | exact — `@param root` doc text |
| spec :634-635 (was :635-636) | lands on the claim's first line — see n1 |
| `spec :456-470` + `splice c := 3` at `:784-786` | exact — M13's four edges (:456-472) inside; "A `splice` is never a place … `splice c := 3` is a check error" at :784-786 verbatim |
| `static` baseline `spec :952-960` (was :952-955) | exact — lower bullet :952-957 + the `choiceDepth`/`outstandingMarks` refusal :958-960 |
| `arena_manifold.hpp:616-618` | exact — unlink→link(nullopt) |
| `store.cpp:1066` / `arena_manifold.cpp:1781` / `:2030-2042` | exact — `B` mint, `discover(edge.dim)`, `real`-resolved `setLink` |
| `spec :736-738` peeling | lands — sentence spans :737-740; see n2 |
| `spec :1300` repoint | exact |

## My r4 nits — both applied verbatim

- "nineteen cells, eighteen filed names (`home` is a plain genesis cell, not a dimension)" (:400) —
  verified: `home` = `arena_.makeCell("home")` (`vortex_core.cpp:35`), `d.dims` = `makeCell` + the
  self-link at :44-46, and exactly 17 `mintNamedDimension` calls `d.grab`…`d.role` (:50-69). 17 +
  d.dims = 18 filed; +home = 19 cells. ✓
- `dimDims && home` conjunct restored (:395, `:91-103`) — the gate `if (dimDims && home)` is
  `arena_manifold.cpp:91-93`, the rank append runs :94-102; `:91-103` covers the guarded block. ✓

## MINOR

### m1 — `spec :2397-2402` is cited for "not a per-checkpoint accrual" while stating the opposite rate

- **Plan ref:** :414 ("— not a per-checkpoint accrual (spec :2397-2402; today's unfiled session
  mints include …)").
- **Spec:** :2398-2399 — "an *unfiled* session-minted dim in use accreting one such orphan **per
  checkpoint** off the folded `d.dims` rank". The cited range's own text states the rate the plan
  denies. The plan's claim is *correct* — the spec's rate describes the spec's once-only
  spawn-seed filing (:2294-2299), while the plan's per-cycle sweep (":404-407", "the same
  name-table sweep runs before each `rebuildManifold`") bounds the unfiled window to one fold —
  but a reader who follows the cite meets the contrary wording with no signpost.
- **Fix:** keep the cite (the orphan mechanism — promoted twin, `*handleTarget`, dead weight — is
  all inside :2397-2402) and disambiguate in line, e.g. "not a per-checkpoint accrual — the spec's
  `per checkpoint` rate is the unfiled-forever case; per-cycle filing bounds it to one (spec
  :2397-2402; …)". Not a correctness defect — the plan's model is self-consistent and better than
  the spec's — hence MINOR not MAJOR.

## NIT

1. **`spec :634-635` (plan :441)** — :634 is blank; the emphasis sentence spans :635-636
   ("`promote` walks only what the agent / cell reaches"). The applied range is vortex n1's
   suggestion, which was itself one line early — :635-636 was strictly better. Lands on the
   claim's opening line, so starting-coordinate-acceptable.
2. **`spec :736-738` (plan :108)** — the `splice f(args)`/peeling sentence spans :737-740; the
   cited range ends one line before "(the peeling rule)" at :739. Same adjacent-range class.
3. **`spec :3020-3026` (plan :391)** — anchors `getOrCreate`/`Store::makeDimension`, but that
   mechanism is :3028-3031; the cited range covers the fixed-list tail of the same diagram block.
   Pre-existing (unchanged v4 text), adjacent lines.
4. **`spec :1870-1871, :2465` (plan :448)** — covers `d.code`/`d.patch`/`d.synapse` on the agent
   cell (:1870-1871 verbatim) and `d.code` again (:2465); the fourth named rank `d.vars`-on-agent-
   cell is best anchored at spec :1294-1295 ("the same per-agent counter on the *agent cell's*
   `d.vars` rank"), uncited. Claim holds; cite coverage is three of four.
5. **Phase-3 graft-arm residue (:273-274)** — still says "`run`/`graft` on a `Code`-parameterized
   (splice-only) fragment is a named refusal" inside phase 3's member list, where `graft_*` has
   nothing to attach to until phase 4. Phase 2's split (:177-179 — "`run` … phase 3, `graft` …
   phase 4", meta m9 applied verbatim) now scopes the delivery, so this reads as the shared rule
   statement rather than a phase-3 promise; noted as residue only.

## Majors-only pass — clean

- No dependency inversion introduced: the agent-cell root narrows promote; (d) replay already owns
  program-side persistence; the image-seed carve-out preserves lineage seeding.
- Exit criteria unchanged and still testable (the ≥2 re-geneses exit depends on agent-side
  `patch:<n>` re-mint, unaffected by the root ruling).
- Coverage matrix closed — no §10.1 row, §10.2 register entry, or seam dropped in the edit.
- Spot-verified unchanged-but-load-bearing cites inside edited paragraphs: `store.cpp:3836-4018`,
  `mesh_value.cpp:473` (`d.op-label` at :473 exact), `or_parallel.cpp:181-184` (four
  `ensureDimension` inits :181-184), `compiler.cpp:769-772` (`dimConstant`/`findOrMintDimension`),
  `arena_manifold.cpp:289-290` (`d.stores` :289 / `d.store-refs` :290), `publication.cpp:1093-1103`
  (`make_unique<Store>()` at :1097), `store.cpp:123` (no-id `Store()` ctor),
  `dimension_registry.cpp:194-217` (rebuilt fold move-assigned :214 — "IS the injected base").
