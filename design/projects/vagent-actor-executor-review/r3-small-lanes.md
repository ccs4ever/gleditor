# r3-small-lanes — combined freeze pass on `design/vagent-actor-executor.md` v0.3

Four lane sections: **ERGO** (implementer completeness), **META** (staging semantics),
**WRITER** (document coherence), **ZIGZAG** (topology/mark-accounting). Scope per
`r2-disposition.md` round-3: the corrected §3.2 exit protocol, the three §3.8 rulings, the
M2 contract additions. Landed r1/r2 dispositions not re-opened; findings are in v0.3 text.
Verified against `apps/common/xanadu/{vortex/vortex_vm.cpp, zigzag/arena_manifold.{hpp,cpp},
vagent/{meta.cpp, lineage.hpp, genesis.hpp, fragment.hpp, compiler.{hpp,cpp}, ast.hpp},
zigzag/manifold.{hpp,cpp}, vortex/mesh_actor.cpp}` and siblings `vagent-agent-language.md`
(:323-372, :596-615, :1479-1500), `vagent-metaprogramming.md` (:271-278, :2808-2835).

______________________________________________________________________

## ERGO — implementer completeness

### Assigned-question answers

**1. Is the corrected exit protocol implementable exactly (cut(1)→release→backtrack)?**

As literally sequenced, **yes** — every named call exists and is public
(`pushChoicePoint` `vortex_vm.cpp:279-309`, `cut` :336-354, `backtrack` :311-334,
`clearChoicePoints`=cut(0) :360, `release`/`discard` `arena_manifold.cpp:1596-1699`), and I
traced the floors arithmetic end-to-end: commit `cut(0)→discard(m0)` ends at
`choiceDepth==0 && outstandingMarks()==0` exactly, matching `checkpointView`'s predicate
(`mesh_actor.cpp:834-836`). But the written sequence is correct **only under an unstated
zero-writes invariant**, and the step list does not pin the sub-order on which that
invariant depends — finding E1. This is not "thin"; it is wrong-adjacent: an implementer
reconciling the text with the sibling paragraph the doc itself cites produces silent arena
corruption on every failed turn.

**2. ANF-hoist — is temp-binding scope/naming specified enough?**

Mostly implementable: a hoisted call becomes an ordinary host step whose result name *is*
the temp; the existing ABI marshals it onto `+d.vars` identically (`meta.cpp:736-756` is
the channel), and step-10's scope unlink bounds temp lifetime to the turn. Unspecified but
inventible: temp naming (the `let ?b :=` illustration uses logic-variable spelling — temps
must be emitter gensyms or they collide with user vars), emission order for multiple
nested calls in one expression (left-to-right source order, unstated), and verb-inside-verb
nesting (`plan_named(symbol_named(…))` — iterative hoist, unstated). The uniform boundary
rule holds for every gate statement. MINOR (E5).

**3. Belief channel — is "implicit arg on +d.vars" implementable?**

**Yes.** The evaluator binds the agent cell on the cursor's `+d.vars` *before*
`runFragment`; the fragment reads it via `#RESOLVE`'s name-keyed walk — the same channel
`runFragment` uses for declared args (`meta.cpp:740-756`). It must not ride the arg vector
(exact arity check, :710-713), which is precisely what "on `+d.vars` (implicit arg)" says.
A pre-call binding survives `runFragment`'s scope restore (`priorVars` recorded :725,
restored :762-771). Unstated: the binding's reserved name (a user param named `agent`
collides innermost-first) and which evaluators beyond the `where`-selector owe the binding
(a `run`'d `code"believes…"` head gets none). MINOR (E6).

**4. Remaining gaps (each MINOR per the brief):**

- **E2** — post-landing flow unstated: after `backtrack(cursor)` lands, do steps 9–10
  (drain/post) run? `release(m0)` destroyed the staged outbox, but `fail_step` itself may
  stage fault notifications that need the drain. And does `fail_step` run under a fresh
  mark or bare? After the landing `outstandingMarks()==0` — its writes are untrailed and
  commit directly (convenient for fault records, but a hard failure *inside* `fail_step`
  has neither mark nor pad). One clause each.
- **E3** — the pad's `pushChoicePoint` arguments are still unnamed: `altOp` must be a
  valid opcode cell or the call throws (:285-288) — which cell carries `std:agent`'s
  step-failure routine, and is the CP bound to the actor cursor or `nullopt` (each wrong
  choice has a §5.1 consequence). M1 inherits the item unnamed.
- **E4** — the sibling's two non-init arms are still absent: ordinary non-intention event
  failures drop + count in `$stats` (`vagent-agent-language.md:367-368` — written *after*
  the undo so the count survives), and `ask`/delegated-`achieve` turns owe a typed-fault
  reply (`remote_fault`, :366-368). v0.3's universal "Failure inside 2–7 → fail_step" arm
  implies `fail_step` for an ordinary handler failure where the sibling drops and counts.
  r2 finding 2's residual — the rewrite fixed the init arm but deleted the drop arm
  entirely rather than correcting it.
- **E7** — edge cases: a failure inside step 2 *before* the push leaves `backtrack`
  returning `false` with no landing (unstated); `cut(1)` "releasing their marks" (:194)
  misnames discard semantics (`cut` calls `discard(cp.mark)` :340 — keeps writes, abandons
  the floor; `release(m0)` then does the actual rollback — moot under the sequence but the
  wording still blurs the distinction this doc spent two rounds fixing).
- **E8** — carried r2 residuals still in text: `decode` helper internals, `$turn` record
  shape, delivery-queue entry shape, `d.seen` append/evict op names, bare
  `belief_event_depth`/`compact_every` knob spellings, `msgStreak`'s role, per-kind outbox
  fields + drain order. Spec-depth, not correctness.

### Finding E1 — MAJOR — the exit protocol is implementable only under an invariant the step list does not pin, and the cited sibling violates it

**Location:** §3.2 steps 1–2 (:170-173), failure bullet (:194-204), M1 (:492).

The sequence `cut(1) → release(m0) → backtrack(cursor)` works iff **no arena write or mint
occurs between `mark()` (step 1) and `pushChoicePoint`'s internal `mark()`**
(`vortex_vm.cpp:302`). Mechanism: `release` pops `floors_.back()` unconditionally
(`arena_manifold.cpp:1695-1697`) — floor *identity* is never compared — so `release(m0)`
under `[F_m0, F_mB]` consumes the base CP's floor slot while applying m0's rollback, and
`backtrack`'s `release(mB)` then resizes `slots_`/`links_`/`content_`/`scratch_`/`trail_`
to mB's recorded extents (:1657-1674). Under zero gap-writes `E_B == E_m0` and the resize
is a no-op; under any intervening write the arena **grows with default entries** — phantom
cells and trail records, invisible to the step-8 counts assert.

v0.3 never states this. Worse for the implementer, step 2 enumerates three sub-actions —
"push the base CP; reset the Dewey path; establish per-turn scope" — without pinning their
order: scope edges written *before* the push put trailed writes in the gap and break it.
And the sibling text the bullet cites — `vagent-agent-language.md:341`'s "drain idiom" —
prescribes the *different* mechanism (`clearChoicePoints()` + `release(m0)` + opcode
reset, :341-343) under a step order that writes `$turn` before its CP push (:332-336): an
implementer harmonizing the two orderings reintroduces the gap-write. The parenthetical
"its entry lives inside m0's span so a failed turn releases it too" justifies the ordering
with a mechanism that isn't what happens (the `choiceStack_` entry is VM state `release`
never touches; what the failed turn consumes is the CP's *floor slot*).

**Fix:** state the invariant explicitly — "the base CP's mark must record extents
identical to m0's: no mint or trailed write may intervene between `mark()` and
`pushChoicePoint`; the step-2 scope edges come *after* the push" — plus a debug assert
`cp.mark == m0` extents at push time, and correct the provenance (the landing-pad
mechanism's actual sibling precedent is `:355-356`'s `backtrack(cursor)` contract arm plus
`:334-336`'s alternative-is-fail_step; `vagent-metaprogramming.md` §6.4 is the drain
idiom). Cross-lane: the vortex lane rates this MAJOR (with a preferred re-ordering —
CP-first — that removes the invariant entirely); the codebase lane rates the package
BLOCKING on the false-provenance arm.

VERDICT (ERGO): NOT CONVERGED

______________________________________________________________________

## META — staging semantics

### Assigned-question answers

**1. Registered-head names vs `priorAttachName` — is the uniform channel named correctly?**

**Verified in code, still unnamed in spec.** `priorAttachName(clauseName, attachDim)`
(`meta.cpp:1618-1644`) walks `binds_.agentCell`'s `+d.patch` rank newest-first, matching
the patch's `"target"` field to the clause's `+d.name` and returning head-names[1] for
plan/handler spin attaches or head-names[0] for `learn_body` — exactly the
recorded-name→registered-name channel the §3.5 contract needs, and `learnWhere` itself
uses it for the grab arm at :1820. The fallback (`nullopt` → `labelOf(+d.spin target)`)
covers declared/image-original bodies. v0.3 :260 still says the recorded identity is
"`patch:<n>`/`codeNamed` names for installed copies" — the r2-flagged conflation
(`patch:<n>` is the *clause/patch* label — `learnPlan` mints it onto the clause's `+d.name`
at :1732-1734; `codeNamed` is a resolver, not a name kind) survives verbatim. Carried
MINOR — deferred by the r2 disposition to implementation notes; recorded here for
completeness, plus a reminder the derivation one-liner was never written (M1).

**2. `+d.owner` emission vs `clauseHost`?**

**Distinct and consistent** — `clauseHost` (`meta.cpp:1167-1192`) resolves a goal/event
*name* to the host symbol cell (the `+d.clause` rank owner); the `+d.owner` edge names the
agent *decl* symbol — orthogonal axes, no conflict with `agentOwner` (:1194-1203, a
name-resolution for graft targeting, not an edge). **New gap (M3):** the emission contract
is M0-only ("M0 emits it", :344), but `learnPlan`/`graftPlan`/`learnHandler` mint clause
cells mid-turn (:1731-1741, :1396-1407) with no `+d.owner` stamping assigned to the learn
verbs — an ownerless learned clause makes "dispatch keys on it" undefined in a multi-agent
module: matches-every-agent is exactly the silent stealing the edge exists to prevent;
matches-none silently loses the learned clause. The learn verbs need an owner source (the
calling actor's decl, or an explicit parameter). MINOR — gate modules are single-agent.

**3. Fragment purity vs graft_consume — does a pure fragment satisfy `learn_where`'s storage path?**

**Yes, verified.** `learnWhere` gates the test through `learnGate(test,
/*pureRequired=*/true)` (:1810) → `headShape` requires the descriptor's `pure` flag
(:1246-1251); the M6-narrowing bans impure `code"…"` at *check* time, so anything reaching
the verb already carries the flag the storage path demands — check-time refusal and
install-time flag are aligned, not in tension. The claim that the ruling "narrows M6" is
accurate (`vagent-metaprogramming.md:271-274` explicitly allows `adopt`/`emit`/`believe`/
`install`/`graft`/`run` in `code"…"`). One honesty note: `learnGate` has no `validate` arm
(:1272-1285, deliberate — closedness is genesis's job), so a verb-*assembled* head whose
descriptor falsely claims pure would pass; the flag's producer is the emitter. NIT.

**4. The reify-redraw fix — verified stable?**

**Verified stable end-to-end.** Genesis (d) replay re-enacts verbs minting *no* patch
records and re-mints `patch:<n>` labels verbatim while each install copy's own
`reify:<src>:<serial>` re-draws (`genesis.hpp:30-34`); the registered head rides the agent
cell's `+d.code` rank (`registerHead`, `meta.cpp:1600-1604`) and its label is durable;
`mintPatch` records registered-head *names* ("durable identities are names, never
CellRefs", :1673-1688). So the recorded comparand (the registered head's label) survives
checkpoint→resume→P16 while the `+d.spin` target's own reify label legitimately changes —
the §3.5 reasoning is mechanically sound.

**5. M2 contract additions** — "`$step` via segment descriptor (not a formal —
`signatureOf` arity)" verified: `signatureOf` builds from descriptor fields
(:1143-1165) and `runFragment`'s arity check is exact (:702-713); a `$step` formal would
fail both. Per-segment Return-tail matches the `Return`-tails convention and the
frame/landing machinery (:726-734, `vortex_vm.cpp:1019-1035`). Belief term-cell encoding
(`+d.grab` term heads) verified `#Unify`-consumable per the Vlog term shape.

### Carried residuals (not re-opened)

The agent-cell `> imageExtent` pin (its own posward Clears need it — `unlinkRank` tail
arm, `records.hpp:46-52`) is still unstated in M0; `meta.cpp:1850-1851` still cited for
`graft_body` whose repoint is :1345-1346 (:267); `d.clone`'s rationale still cites the
pre-filed dim and "recipe dims in `kAgentDimensionNames`" still names a constant table for
data-dependent names (:479-480); the "re-enacts without re-recording" replay discipline
and the `fault:`/`unlearn:` tombstone asymmetry remain unwritten; `d.seen`'s
append-pair/one-op-eviction still unnamed.

VERDICT (META): CONVERGED

______________________________________________________________________

## WRITER — document coherence

### Assigned-question answers

**1. Step-list coherence (1–10 vs §8 refs): CLEAN.** The renumbered list (:170-191) is
consistent with §8's "steps 1–10 of §3.2" (:489), with "Failure inside 2–7" (:194) covering
exactly the work span (steps 8–10 are post-decision, covered by §4.4's adapter-bug rule),
and with M1's shorthand `commit(cut(0)→discard) / fail(cut(1)→release→backtrack)` (:492).
A grep for stale `step 0`/`0–9`/`1–6` references finds none. The scope edges named in the
failure bullet correctly attribute to step 2.

**2. The three §3.8 rulings vs the §9 decisions list: NOT RECORDED.** §9 (:514-540) is
unchanged from v0.2 — "Owner-ratified" (3 items) and "Forced rulings" (6 items) contain no
ANF-hoist, fragment-purity, or belief-channel entries. The header's "flagged for owner
confirmation (§3.8, §9)" (:5) cites §9 for a class §9 has no subsection for — the pointer
half-dangles. **Fix:** a three-row "Pending owner confirmation" block in §9 (or move them
under "Forced rulings" with a pending marker).

**3. "Flagged for owner ratification" vs §9's structure: inconsistent.** The doc's own
taxonomy has exactly two ruling classes (ratified / forced); the r2 rulings are a third
state — spec-level forced, owner-unreviewed — that exists in the header (:4-5) and §3.8's
intro (:327) but has no §9 home and no stated post-ratification destination (forced list?
a fresh class?). Process-wise the gesture is adequate for this round; the record is not.
Same MINOR as the above.

**4. Miscites and format:** two new miscites in v0.3 text (W1, W3); mdformat
`--check --wrap 100` clean; `mdl -i` clean.

### Findings

- **W1 — MAJOR (cross-lane; the codebase lane carries it BLOCKING).** "This is
  `vagent-agent-language.md:341`'s drain idiom" (:198) is **false provenance**: :341-343
  prescribes `clearChoicePoints()` (cut(0)) + `release(m0)` + manual opcode reset + a
  recovery *short turn* — the arm the next clause declares unusable. "`clearChoicePoints()`
  is *never* used on the failure path" (:199) is false of the contract the doc implements.
  As a citation defect this misdirects every implementer who checks the source; as a
  coherence matter the bullet asserts a universal the sibling violates on its face. The
  actual precedents for the landing-pad mechanism are `:355-356` (backtrack lands in the
  failure path) + `:334-336` (base CP alternative = fail_step), and the real drain idiom
  is `vagent-metaprogramming.md` §6.4.
- **W2 — MINOR.** §9 does not record the three flagged rulings; "(§3.8, §9)" half-dangles;
  no pending-confirmation class exists (above).
- **W3 — MINOR.** "reserved fuel (§4.3)" (:197) and "one aggregate condition-fuel account
  (§4.3)" (:181) both point at the settle-deferral section; the reserve and the account
  live in §4.2's first and third bullets (:362-367). "(coalesced, §4.1)" (:176) should be
  §3.3's wake paragraph. (The §4.3 cite at :233 for the `condition` fault is *correct* —
  §4.3 does own the fault.)
- **W4 — NIT.** `let ?b := believes(…)` (:333) — `?`-names are unification vars
  (`vagent-agent-language.md:686-702`); a gensym temp reads truer for ANF. Header :5's
  "(§3.8, §9)" see W2.
- **Carried, still verbatim (one-line reminders):** `meta.cpp:1850-1851` learnBody-only
  cite for `graft_body/learn_body` (:267); `vortex_vm.cpp:745-829` truncates `Linked`
  (:155 — runs to :851); `:549-551` is the catch-map, the stop is `:612-617` (:204); the
  11-vs-7 question arithmetic (:16 vs :469); bare `§9.2`/`§9.3` self-collisions (:266 et
  al.); U-A8 label mismatch (:540); "Forced rulings" overstates items 4 and 6; §6.3's
  paraphrased quote (:245); §3.7's unstated drain order; the
  `vagent.max_live_cells`→grant pipeline; auto-reply glosses the `ActorDown` failure arm
  (:205-206); `d.owner` collides with the link-package property dim
  (`zigzag/manifold.cpp:1640`, `manifold.hpp:568` — the codebase lane owns this one).

VERDICT (WRITER): NOT CONVERGED

______________________________________________________________________

## ZIGZAG — topology

### Assigned-question answers

**1. Corrected exit protocol's mark accounting.**

- *Does the base CP's mark sit inside m0's span?* **Yes** — `pushChoicePoint` calls
  `arena().mark()` internally (`vortex_vm.cpp:302`), after step 1's `m0`: floors are
  `[F_m0, F_mB]` with mB the innermost floor, extents `E_B ≥ E_m0` componentwise
  (`arena_manifold.cpp:1572-1594`).
- *Does `discard(m0)` after `cut(0)` leave nothing?* **Verified clean.** `cut(0)` pops
  every CP top-down discarding each mark (:336-340) — floor pops are perfectly LIFO under
  the m0-first ordering; `discard(m0)` then pops `F_m0`, decrements to zero and clears
  `trail_` (:1596-1610). End state `choiceDepth==0 && outstandingMarks()==0`, trail empty,
  writes kept. The success arm is exact.
- *The failure arm's accounting — correct but conditional (Z1).* Under `cut(1) →
  release(m0) → backtrack(cursor)`: `release` pops `floors_.back()` unconditionally
  (:1695-1697), so `release(m0)` consumes **mB's floor slot** while applying m0's
  rollback, and `backtrack`'s `release(mB)` consumes **m0's** slot while resizing to
  `E_B` — a crossed but count-equal pairing that is safe **iff `E_B == E_m0`**, because
  `resize` *up* manufactures default entries (:1671-1674). The pairing holds only because
  floor identity is never read — a slots-stack coincidence, not a designed correspondence.
  The spec's parenthetical ("its entry lives inside m0's span so a failed turn releases it
  too") conflates three objects: the `choiceStack_` entry (VM state — survives
  `release(m0)` by construction, `vortex_vm.hpp:237`), the floor slot (consumed by
  `release(m0)`), and the mark extents (released by `backtrack`). Same underlying defect
  as ERGO E1 / the vortex lane's MAJOR — stated here in topology terms.

**2. `+d.owner` edges — direction and promotion.**

- *Declared clauses (M0 emission):* clause and agent-symbol are both in-image cells →
  intra-image edge, sealed and promoted verbatim with the program fold. Direction
  `clause → agent-symbol` is correct (ownership is clause metadata read by dispatch). Not
  program→state — the "agent symbol" is the program-side decl cell on module `d.vars`,
  the only thing that can exist at emission time. ✓
- *Learned clauses:* `learnPlan`/`graftPlan`/`learnHandler` mint clause cells post-image
  (`meta.cpp:1731-1741, :1396-1407`) — state-side refs that never promote anyway (the
  clause is frontier-unreachable from the agent cell; durability rides the `+d.patch` EDL
  replay, which re-mints and re-attaches each genesis). Stamping `+d.owner →
  <agent symbol>` on one would be a state→program edge in V12's letter
  (`vagent-agent-language.md:244` — the same shape §2.1 bans for `+d.weight`, :80-81) —
  inert in effect since the clause never reaches the frontier, but the doc forbids the
  shape rather than pricing the exception. The real gap: no one is assigned to stamp it —
  shared with META M3. MINOR (Z2).

**3. Belief-channel binding — state→program edge hazard?**

**None.** The implicit arg is a `+d.vars` variable cell minted mid-turn with
`+d.values → agentCell` — an ephemeral→state-side edge, the reverse of V12's banned
direction; the fragment's clause-walk performs reads, which mint no edges. The binding
survives `runFragment`'s scope restore (`meta.cpp:724-771`) and is removed by step-10's
per-turn unlink before any checkpoint could see it. Even if retained, the agent cell is
the promote root — nothing is dragged. ✓

**4. `d.verb`/`d.owner` filing order — verified.** Both join M0's filed list
(:479-480) and are used mid-turn — file-at-genesis precedes use, correct order. Neither
exists in `kAgentDimensionNames` today (`lineage.hpp:67-71` — the 13 current entries are
all pre-existing dims), so the list entry is a genuine work item, not a duplicate.
Caveat: `d.owner` is a **taken dim name** — the link-package property layer decodes
`owner` properties on `d.owner` (`zigzag/manifold.cpp:1640`, documented
`manifold.hpp:568`). Filing is idempotent-by-name so no DimRef twin and no shared cell —
but one dim name now carries two unrelated semantics; worth a note or a rename
(`d.agent-owner`). Also still mis-bucketed in the same list: `d.clone` (already filed via
`kGenesisDimensionNames`, `lineage.hpp:48`) and "recipe dims" (data-dependent names — not
constant-table entries; filed via the emission list + genesis-(b) sweep). (Z3, MINOR —
the collision half is the codebase lane's finding too.)

### Findings

- **Z1 — MAJOR.** The failure-path floor pairing is crossed-but-count-equal and safe only
  under the unstated `E_B == E_m0` invariant (mechanics above; the fix and alternatives
  are ERGO E1's). In pure mark-accounting terms: correct as literally ordered, wrong as a
  *claimed correspondence* — the spec should either pin zero writes between `mark()` and
  the CP push or take the vortex lane's CP-first ordering, which makes both releases pure
  shrinks unconditionally.
- **Z2 — MINOR.** Learned clauses have no assigned `+d.owner` stamper, and stamping one is
  a V12-letter state→program edge; the emission contract is M0-scoped. (Shared with META
  M3 — multi-agent modules only; gate is single-agent.)
- **Z3 — MINOR.** `d.owner` name collision with the link-package property dim; `d.clone`/
  recipe dims mis-bucketed in the `kAgentDimensionNames` enumeration.
- **Z4 — NIT.** "releasing their marks" (:194) / "releasing its mark" (:184) — `cut` and
  `clearChoicePoints` *discard* CP marks (keep writes); `release` is the rollback arm. The
  discard/release distinction was the substance of the r2 BLOCKING; the new text still
  blurs it in both arms' parentheticals.

VERDICT (ZIGZAG): NOT CONVERGED

______________________________________________________________________

## Cross-lane summary

The one piece of new mechanism this round invented — the `cut(1)→release→backtrack` exit
— is correct **only under an unstated zero-writes invariant** and cites the wrong sibling
for provenance. That defect surfaces in three of the four lanes (ERGO E1 MAJOR, ZIGZAG Z1
MAJOR, WRITER W1 MAJOR-false-cite; vortex/codebase rate their versions MAJOR/BLOCKING).
Everything else verified clean or carried-minor: the ANF hoist is implementable, the
belief channel is a real channel with a real mechanism, purity is compatible with
`learn_where`'s storage gate, the reify-redraw stability claim is sound, `d.verb`/`d.owner`
are filed, step renumbering is consistent, and mdformat/mdl are clean.
