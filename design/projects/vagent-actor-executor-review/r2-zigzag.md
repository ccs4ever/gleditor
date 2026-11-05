# R2 zigzag critique — `design/vagent-actor-executor.md` v0.2 (freeze pass)

Reviewer lane: cell-graph topology, dimension/rank discipline, filing and promotion ordering,
genesis sequencing. Re-walked against `promote()` (`arena_manifold.cpp:1733-2116`), `link()`'s
shadow/mutual-eviction (`:1338-1378`), `RankView`/`rankTail` (`cell_views.hpp:117-131,:237-262`),
`regenesisCycle`/`fileDimensionVocabulary` (`lineage.cpp:190-205,:112-147`),
`cut`/`backtrack`/`clearChoicePoints` (`vortex_vm.cpp:299-360`), `mark`/`release`/`discard` floors
(`arena_manifold.cpp:1572-1700`), the compact/reap surface (`:1702-1731`,
`arena_manifold.hpp:715-754`), `resolveDimension` (`meta.cpp:806-826`, `genesis.cpp:44-55`),
`codeNamed`/`learnCopies`/`mintPatch`/`priorAttachName` (`meta.cpp:251-284,:1573-1606,
:1618-1693,:1695-1859`), genesis (d)/(e) replay (`genesis.cpp:233-356,:359-464`), and
`kGenesisDimensionNames`/`kMetaDimensionNames`/`kAgentDimensionNames` (`lineage.hpp:40-71`).

Counts: **0 BLOCKING, 2 MAJOR, 6 MINOR, 2 NIT.**

## Disposition verification — what landed

- **B5 (`d.agents` → directory cell): LANDED.** §3.1 (:143-153) now routes
  `home +d.dirs → dirCell +d.agents → agent`, names the bidirectional-frontier refutation
  (`arena_manifold.cpp:1794-1796` correctly cited), and §5.2 owns the cursor mechanism in M0.5.
  Verified end-to-end below (V1) — the fix eliminates the *every-cycle* doc-caused accretion; a
  one-shot bootstrap at the dirCell-minting cycle remains, inside §5.2's scope (R2-3).
- **B8 (`maxLiveCells` owed): LANDED.** §4.6 (:380-387) now states "owed, not cited" — grant field +
  `liveCells()` counter as M0.5 work items. `ActorGrant` still lacks the field
  (mesh_actor.hpp:55-65), now correctly framed as owed. The counter formula needs one clause
  (R2-6).
- **M-d.seen (ring closure): LANDED.** §3.1 (:157-159) states the ring closes through the agent
  cell — verified below (V2). Append/evict op names still unstated (NIT, folded into R2-9).
- **F4 (`+d.evidence` carrier): LANDED adequately.** §3.6 (:276-278) — "links `+d.evidence →` …
  That edge is the ownership that carries the cell through promote (`agent→waiting→event` is
  frontier-reachable)". The "real edge, not int field" reading is now forced by the wording.
- **F5/F6/F7/F8/F9:** F7 landed (§3.1 :111-117 now names `resolveDimension`'s refusal and the raw
  primitive's gap correctly). F5, F6, F8, F9 partially landed or open — see R2-8.

## Verification tasks

### V1 — the directory-cell fix, walked

Frontier under v0.2 (promote root = agent cell, `lineage.cpp:204`):

1. **agent** expands (root exempt from the :1768 prune) → `-d.agents` discovers **dirCell** into
   `order`+`frontier`.
2. **dirCell** expands while ephemeral (always) or when shadowed (persistent `> imageExtent` —
   only on a `holdsOwn` write). Its `dimensionsOf` = `+d.agents` members, `-d.dirs → home`, and a
   `+d.dirs` forward edge only if a second directory appends after it. **dirCell's own emission is
   confined to `+d.agents`/`+d.dirs`** — the spec's claim holds for its diff. But expansion is
   discovery, not just emission: `-d.dirs` discovers **home** into `frontier` whenever dirCell
   expands. The spec's "its expansion commits nothing else" (:150-151) is true of dirCell's diff
   and silent about the reach.
3. **home** expands iff shadowed; emits iff `holdsOwn` (:2055). Under dirCell, home is reached only
   when dirCell expands — and dirCell is shadowed essentially once (its minting/registration
   cycle: `link(dirCell,+d.agents,…)` on the empty rank tails to dirCell itself; later appends
   shadow the tail *member*, not dirCell; tombstone delisting writes the **agent** cell per
   :152-153 — verified no designed dirCell-mutation path leaks). **So the naive form's every-cycle
   home membership is genuinely eliminated.**
4. **Residual bootstrap:** in that one cycle, home is both reached (dirCell expands while
   ephemeral) and shadowed (the `+d.dirs` write itself + `spawnCursor` tailing the still-empty
   durable `+d.cursors` rank to home, `vortex_vm.cpp:242-245`) → home expands → `+d.cursors →
   cursor` commits under Additive → twin₁ durable. Thereafter the chain is self-sustaining
   **without home**: `spawnCursor` shadows the tail twin (`> imageExtent` → `extraRoots`,
   `lineage.cpp:190-195`) → twinᵢ emits `+d.cursors → cursor_{i+1}` → twin_{i+1} durable. This is
   exactly the mechanism §5.2 owns (Suppress-policy rank, or genesis unlinking dead twins — both
   kill the bootstrap too). **Separation: clean** — the doc eliminated the *recurring* home
   membership; §5.2 owns the *mechanism* that makes any single home expansion commit the rank.
   Caveats: §5.2's "`home` is a base cell always in the promote fold" (:409) is imprecise — home is
   reached only when dirCell expands (today, never); and M0 lands the dirCell before M0.5 lands
   the §5.2 fix, so the bootstrap fires once in the interim (the unlink path cleans retroactively).
   → R2-3.

### V2 — ring closure, verified

`RankView` stops at dead-end/self-link/return-to-start/`traversalBound()+1`
(`cell_views.hpp:121-131,:157-164,:203`). A journal ring `j1→…→jK→j1` not through the anchor lassos:
`rankAfter(agent,d.seen)` yields `traversalBound()+1` duplicate entries and `rankTail` an arbitrary
member — the claimed hazard is real. The stated shape (`agent +d.seen→first`; `last +d.seen→agent`)
walks `agent→j1→…→jK→agent` — stops at start after exactly K members; `rankTail` = jK ✓. Head
eviction is one `link(agent,+d.seen,j2)` — mutual eviction clears j1's backlink (`:1347-1373`); an
evicted durable journal cell re-enters via `extraRoots`, splices under Clears, drops; an evicted
ephemeral simply never mints. `Linked` is safe either way (seen-set, `vortex_vm.cpp:827-831`). The
`await` chain `agent→waiting→event` keeps evicted cells frontier-reachable exactly as §3.6 says ✓.

### V3 — §3.5 named-identity, verified

The durable channel is names, confirmed: `mintPatch` records registered-head *labels*
("durable identities are names, never CellRefs", `meta.cpp:1673-1688`); genesis (d) re-resolves
via `codeNamed(heads[index])` (`genesis.cpp:247-262`) and re-mints `patch:<n>` clause labels
verbatim (`:287-292`); install copies re-draw `reify:<src>:<serial>` per replay (`meta.cpp:1101-1105`,
`genesis.hpp:30-34`). So a recorded **registered-head name** survives checkpoint→resume→P16 while
the +d.spin target's own label (install copy) does not — the spec's reasoning is correct.
Resolution channel exists: `priorAttachName(clauseName, d.spin)` walks the newest patch's
head-names (`meta.cpp:1618-1644`) — unstated in the spec; and the gloss "`patch:<n>`/`codeNamed`
names for installed copies" mislabels — a registered body head carries a `reify:*` or declared
name (`patch:<n>` is a clause/patch label). → R2-5.

### V4 — M0 dim filing vs v0.2 usage

Every dim in the text files under the fixed arrays (`lineage.hpp:46-71`) **except `d.verb`**
(§3.8 ABI, :316) — absent from `kAgentDimensionNames`'s M0 enumeration (:454). `d.clone` is
already a genesis dim. → R2-4, R2-7.

### V5 — `liveCells()` formula

`compact()` repacks link/content runs, never slots (`:1702-1731`); `release()` truncates under a
mark — nothing reaps. "minted-minus-reaped" needs the decrement named. → R2-6.

## New findings

### R2-1 — MAJOR — §3.2 step 7: the success arm omits `clearChoicePoints()`; its own assert can never hold

- Spec location: §3.2 step 7 (:182): "`commit: discard(m0) → writes keep, marks release; assert
  choiceDepth==0 && marks==0`".

- Problem: `discard(m)` pops **one** floor (`arena_manifold.cpp:1596-1609` — `outstandingMarks_--;
  floors_.pop_back()`) and touches nothing in `choiceStack_`. Per-statement CPs pushed during steps
  4–6 stay live on success — and the step-0 base CP *always* stays live. So after a literal
  `discard(m0)`: `choiceDepth() ≥ 1` and `outstandingMarks() ≥ 1` — the line's own assert fires on
  **every** successful turn, deterministic or not; §4.4 makes that an adapter bug, not a user
  fault. The sibling prescribes the missing op verbatim — "`clearChoicePoints()`, then
  `discard(m0)`" (`vagent-agent-language.md:341`) — and v0.2's own failure bullet names
  `clearChoicePoints()`, so the omission reads as an edit slip in the §3.2 rewrite, not intent.

- Evidence: `arena_manifold.cpp:1596-1610` (discard = one floor), `vortex_vm.cpp:336-360` (CPs are
  VM-side; `discard` doesn't see them), `vagent-agent-language.md:341`.

- Fix: `7. commit: clearChoicePoints() → discard(m0) → writes keep, marks release; assert
  choiceDepth==0 && marks==0` — the clear first, while CP marks are still well-nested above m0.

### R2-2 — MAJOR — §3.2 failure bullet: `clearChoicePoints()` (cut(0)) destroys the "surviving base CP" `fail_step` supposedly runs against

- Spec location: §3.2 (:191-197): "`clearChoicePoints()` (backtrack-drain to the step-0 base CP,
  which releases every per-statement mark), then `release(m0)` … `fail_step` runs against the
  surviving base CP".

- Problem: `clearChoicePoints()` **is** `cut(0)` (`vortex_vm.cpp:360`) — it drains *all* CPs
  including the step-0 base CP. No base CP survives to run `fail_step` against. The three clauses
  cannot be satisfied by one implementation: the named function does cut-to-empty, the
  parenthetical describes cut-to-base (that's `cut(1)` — `cut` is public, `vortex_vm.hpp:176`), and
  the final clause requires the base CP alive. Two more slips in the same bullet: `cut`
  **discards** each CP's mark (`:336-341` — keeps writes; the rollback is `release(m0)`'s or the
  landing `backtrack`'s `release(cp.mark)`, `:319`), so "releases every per-statement mark" misnames
  the mechanism; and the sibling's actual hard-failure arm — `clearChoicePoints()`, `release(m0)`,
  **reset the cursor's opcode**, run recovery (`vagent-agent-language.md:341-343`) — contains no
  "surviving base CP" and adds a cursor-opcode reset v0.2 omits (the released turn may have
  truncated the PC's edge). The base CP's real job — its alternative *is* the step-failure routine
  (sibling :335-337) — belongs to the in-run backtrack cascade, a different level than this
  executor arm; the bullet conflates them.

- Evidence: `vortex_vm.cpp:311-360`; `arena_manifold.cpp:1596-1697`; `vagent-agent-language.md:
  335-343`; floors well-nesting `arena_manifold.hpp:841-849`.

- Fix (minimal, keeps the spec's own semantics): `clearChoicePoints()` → **`cut(1)`** — drain the
  per-statement CPs, keep the base CP — then `release(m0)`, then `backtrack(cursor)` lands on the
  base CP: it releases `m_base` (`outstandingMarks_→0`) and installs the step-failure alternative,
  so `fail_step` runs *as* the surviving base CP's continuation under reserved fuel. Alternative:
  adopt the sibling's literal arm (`cut(0)` → `release(m0)` → reset cursor opcode → run recovery as
  fresh post-rollback work) and delete "surviving". Either way, also correct "releases" →
  `cut`/`discard` semantics and add the cursor-opcode reset.

### R2-3 — MINOR — the dirCell bullet overclaims its isolation; §5.2's wording and the §6.6 amendment trail behind

- Spec location: §3.1 :143-153 ("its expansion commits nothing else"), §5.2 :406-414, §9 :500.

- Problem: (a) dirCell's expansion still *discovers* `home` (frontier is bidirectional,
  `:1794-1796`) — only its emission is confined. In the minting cycle home is inevitably shadowed
  (the `+d.dirs` write + `spawnCursor`), so the `+d.cursors` bootstrap fires exactly once before
  §5.2's mechanism takes over — worth one sentence so the M0.5 dependency reads as load-bearing,
  and so a reader doesn't conclude the dirCell made home unreachable. (b) §5.2's "`home` is a base
  cell always in the promote fold" is false as stated — home is reached only when dirCell expands
  (under this doc: ~once; today: never). (c) `vagent-metaprogramming.md:1804-1814`'s invariant —
  "no *system-minted* edge creates a link path from the agent cell to `home`" — is still violated
  in letter by `agent −d.agents→ dirCell −d.dirs→ home`; the design's safety now rests on dirCell
  being *write-quiet* (behavioral), not on path-absence. The amendment my r1 asked for is still
  unowned (the "Unbundled paperwork" line covers §6.3/user-guide only). (d) Relatedly, the reason
  tombstones *must* sit on the agent cell — any `Clears`-style write on dirCell re-shadows it and
  re-admits home — is the load-bearing "dirCell must never be shadowed after mint" rule; the
  forced ruling states the outcome, not this reason.

- Evidence: `arena_manifold.cpp:1768-1796,:2054-2068`; `lineage.cpp:190-205`; spec :143-153,
  :406-414, :482-483, :500; `vagent-metaprogramming.md:1804-1814`.

- Fix: one sentence in the dirCell bullet ("expansion still reaches `home` once, at mint —
  §5.2's fix is what makes that visit commit nothing"), correct §5.2's "always" → "whenever it is
  reached," and add the §6.6 amendment to the paperwork line (permit the agent↔dirCell↔home path
  *because* dirCell is write-quiet; name the no-shadow-after-mint duty).

### R2-4 — MINOR — `d.verb` is used in §3.8 but missing from M0's filing enumeration

- Spec location: §3.8 :316 (`+d.verb → <verb-name>`), M0 :454 (filing list).

- Problem: M0 files `d.agents`/`d.dirs`/`d.seen`/`d.evidence`/`d.clone`/recipe dims in
  `kAgentDimensionNames`; `d.verb` — the only other new dim v0.2 introduces — is absent. In
  practice `dimensionVocabulary(arena)` sweeps `arenaDims_` (`lineage.cpp:112-131`), so an
  in-arena-minted `d.verb` files transitively; but the enumerated arrays exist precisely to make
  resolution arena-independent (a dim minted in a non-swept arena, or needed mid-turn where the
  `resolveDimension` refusal bites, `:818-825`). An unfiled `d.verb` resolves as `noCell` → the
  host-step discriminator reads no verb edge → a host-step segment is treated as a halt — silent
  ABI loss (loudly caught by §10's M2 round-trip test, but the work item should be complete).

- Evidence: `lineage.hpp:64-71`; `lineage.cpp:112-147`; `meta.cpp:818-825`; spec :316, :454.

- Fix: add `d.verb` to the filing list (it is a `std:agent` surface dim like the rest).

### R2-5 — MINOR — §3.5's name gloss and the unnamed install→registered resolution channel

- Spec location: §3.5 :256-261; M3 :476.

- Problem: (a) "`patch:<n>`/`codeNamed` names for installed copies" — a learned body's registered
  head carries a `reify:<src>:<serial>` label (or a declared `code"…"` name under the
  already-registered carve-out, `meta.cpp:1579-1588,:1101-1105`); `patch:<n>` labels live on
  clauses and patch records, not heads. An implementer recording the clause's `patch:<n>` would
  compare it against the body's registered name and fault every resume. (b) The resolution channel
  — current install copy → its registered name — is unstated: for learned clauses it is the
  `+d.patch` head-record walk (`priorAttachName`, `meta.cpp:1618-1644`: newest body-touching patch,
  index 1 for plan/handler, 0 for `learn_body`); for declared clauses it is `labelOf(+d.spin)`
  directly. M3's "bound-body-name recording" should name this procedure. (c) The M0-emitted
  declared body-head names must be unique per head — unstated; a collision (two clauses sharing one
  declared body name legitimately, or duplicate emission) makes a replaced body undetectable. The
  check itself is otherwise sound: recorded-vs-resolved mismatches fire exactly on `learn_body`/
  `graft_body` repoints and detached clauses, as spec'd.

- Evidence: `meta.cpp:1573-1606,:1618-1644,:1695-1749,:1832-1859`; `genesis.cpp:247-262,:287-321`.

- Fix: restate as "the name `labelOf` returns for the +d.spin target's *registered source head* —
  resolved through the newest body-touching `+d.patch` record for learned clauses, the head's own
  `+d.name` for declared ones (M0 emits unique declared body-head names)".

### R2-6 — MINOR — `liveCells()`: "minted-minus-reaped" names a decrement nothing performs; two spellings of the bound

- Spec location: §4.6 :380-387; M0.5 :463.

- Problem: nothing in the substrate reaps cells — `compact()` repacks `links_`/`content_` runs and
  leaves `slots_` untouched (`arena_manifold.cpp:1702-1731`), `release()` truncation is rollback,
  and the durable base is append-only. As specified, the counter is monotone and the bound
  unenforceable except at the (owed) drain-mode GC. The coherent definition needs the decrement
  named: e.g. "durable cells dropped by a checkpoint/rebuild + ephemeral mints not yet rolled
  back," with tombstoned cells counting as live until reclaimed (the honest accounting — a
  tombstone is a read-filter, not a free). Separately, the invariant is spelled
  `vagent.max_live_cells` (a settings name, consistent with §4.2's ruling) while M0.5 says "grant
  gains `maxLiveCells`" — two homes for one bound; state the relationship (grant = capability
  ceiling fed by the setting, or the setting is the bound and the grant field redundant).

- Evidence: `arena_manifold.cpp:1702-1731,:1596-1697`; `arena_manifold.hpp:548,:715-754`;
  `mesh_actor.hpp:55-65`; spec :346-352, :380-387, :462-463.

- Fix: one clause naming the decrement event and reconciling the two spellings.

### R2-7 — MINOR — `d.clone` premise stale: it is already a filed genesis dim

- Spec location: §3.1 :115-117; M0 :454.

- Problem: "`d.clone` … must be filed at genesis too, since `d.clone` is minted lazily 'outside
  marks' today (`meta.cpp:34-38,:967`) and a mid-turn `learn_*`→`reify` would refuse." But `d.clone`
  is minted unconditionally by `initGenesis` (`vortex_core.cpp:57`) and sits in
  `kGenesisDimensionNames` (`lineage.hpp:48`) — `resolveDimension` hits the `findDimension` arm
  first (`meta.cpp:818`) and resolves it under marks; no mint, no refusal. The lazy path the
  comment describes fires only if the filing were absent. Listing it is harmless (filing is
  idempotent by name) but the justification misreads the mechanism it cites.

- Evidence: `vortex_core.cpp:57`; `lineage.hpp:46-52`; `meta.cpp:34-38,:818-825,:967`.

- Fix: strike the clause or restate it as "already filed; the mid-turn-refusal risk applies to any
  *new* dim a verb names — hence the filing list."

### R2-8 — MINOR — r1 minors still open in v0.2

- **F5 (delisting mechanics):** the agent-cell location and `unlearn:` convention are stated
  (:152-153), but the tombstone's **key spelling** (a stable `delisted:`/`unlearn:`-style key —
  `varsRecord` dedupes on key text, `meta.cpp:323-335`) and the **reader-filter duty**
  (`cells"##/+d.dirs/+d.agents"` returns tombstoned members verbatim; consumers must filter, the
  `clausesOf` precedent `meta.cpp:210-218`) remain unstated.
- **F6 (Clears seat):** "(state-side `Clears`)" gestures at the policy but the seat condition —
  the agent cell must sit `> imageExtent` for its own posward clears (drain/unlink) to commit —
  is still unstated; a ≤E_ops program-side seat silently drops them
  (`arena_manifold.hpp:278-282`, `:2085-2088`).
- **F8 (`d.evidence` namespace):** still filed top-level beside the retained-evidence
  `d.evidence.*` names (`evidence_controller.cpp:1658,:1689,:1761,:1772`) — no rename, no
  acknowledge.
- **F9 (diagram anchoring):** worse, not better — the `│`/`├` rank branches now sit at column 46,
  under the `─(+d.agents)` arrow, while `[agent cell]` begins at column 53 (:120-136); a literal
  read can hang the whole rank tree off the directory cell or the arrow, which the prose forbids.
  One indent to column ≥53, or the "every rank below hangs off the agent cell" sentence, closes it.

- Fix: the four one-line statements from r1, unchanged.

### R2-9 — NIT — small wording/cite slips

- Provenance fields named twice, differently: step 2's "`source/space/time/ordinal`" (:174) vs
  §3.3's "`source/ordinal/requestId`" triple (:208-209) — one spelling.
- "`graft_body`/`learn_body` install a fresh named head — `meta.cpp:1850-1851`" (:263): that range
  is `learnBody`'s repoint; `graftBody`'s is `meta.cpp:1338` (`meta.hpp:370-372`).
- The `d.seen` bullet still doesn't name the append pair (`link(jK,+d.seen,jNew)` +
  `link(jNew,+d.seen,agent)`) or the one-op head eviction (`link(agent,+d.seen,j2)`); the closure
  shape implies them, and naming them costs a clause.

VERDICT: NOT CONVERGED
