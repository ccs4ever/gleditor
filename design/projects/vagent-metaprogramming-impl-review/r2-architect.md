# r2 — system-architecture review (phase ordering and deliverability)

**Verdict:** NOT CONVERGED (near — one MAJOR, a one-arm omission inside an otherwise
correctly rebuilt phase 5)

**Counts:** 0 BLOCKING / 1 MAJOR / 5 MINOR / 5 NIT

Lens unchanged: phase ordering and deliverability. Plan under review:
`design/vagent-metaprogramming-implementation.md` v2 (line refs below). Spec refs are
`design/vagent-metaprogramming.md` v1.19; `vagent-agent-language.md` for V10/§5.8/P4.

## r1 findings — verification

All six r1 findings and all six sub-findings closed; verified against v2 text and tree.

| r1 | Status | Evidence |
| -- | ------ | -------- |
| B-1 (`std:meta` split) | **CLOSED** | Phase 3 now owns the scaffolding + read group — import allowlist (`checker.cpp:2061-2067`, verified), signature-table rows (`checker.cpp:765-779`, verified — the `builtin` member rows live there), prelude skeleton — plus `run`, `reify`, `validate`, `weight_of`/`value_of`/`next_rand`, the `reflect` family and the head-vs-op discriminator (plan :203-215). Phase 4 owns only the mutation verbs (:261-267). Split landed exactly on the read/write boundary the disposition ruled. |
| B-2 (silent preconditions) | **CLOSED** | New "Preconditions" section (:20-48) names the Call ABI (`localCells_` interim shape, `compiler.cpp:853-855` — verified the reserve/push_back shape), the adapter/actor executor, V10's dim mechanism, the linked-prelude path, and P4's wake as *owned* phase-6 work (`vagent-agent-language.md:1670`, `mesh_actor.hpp:81` non-optional `message`, `pending` at `mesh_actor.cpp:659` — all verified). Every phase now carries a **Requires** line splitting kernel-simulable from adapter-gated. |
| B-3 (5↔6 boundary) | **CLOSED** | Boundary re-cut at transport (:12-14): phase 5 owns the lineage `Store`, `historyFromSeal` seeding (`publication.cpp:1093-1103`, `Store()` at :1097 — verified), the span-source pair (`publication.cpp:1129-1131`, `store.cpp:1388-1405` — verified), dim filing, and the (a)–(e) driver. Phase 6 keeps wire seams + P16 only. |
| M-1 (genesis order inverted) | **CLOSED** | (d) `d.patch` replay now precedes (e) rehydration "last pass" with the R7-B rationale cited (:336-350); (f) turns explicitly out of scope. Matches spec :2381-2440. |
| M-2 (unowned items) | **CLOSED, two residuals** | `programRef`/`E_ops` record → phase 3 (:198-202); `setOpcodeKind` spawn-*and*-resume arms → phase 4 registers, phase 5 (b) exercises (:256-260, :331-333); the 12-dim bind set + `validate` constants + roots → phase 3 (:216-223, twelve names match spec :1053-1060 exactly); `staticFuel` → phase 2 (:167-169); full `std:meta` surface incl. `learn_where`/`learn_body`/`set_weight` dispatch/`sprout_synapse` → phases 3-4. Residuals: the `test_of`/`body_of` memo chain and `reify`'s `reify:<serial>` counter are still unbulleted (m-1 below), and a new unowned arm surfaced in (e) — see **M-1** in findings. |
| M-3 (pin placement) | **CLOSED** | The pin is now a named step: "seed → mint/file → fold → pin/inject → first-resolution" stated as phase 5's ordering invariant (:296-300, `getOrCreate` move-assign verified `dimension_registry.cpp:194-217`), continued into phase 6 (:368-373) where seam (i) is "the pin on the spawn lineage" and (ii) "the pin on the resume lineage". Filing precedes the pin on both lineages. |
| m-1 (P15 rationale) | **CLOSED** | :14-18 now says sanctioned mutation is what makes kind claims load-bearing, and phase 3 ships under the same pre-existing unguarded-write caveat. |
| m-2 (`learn_method` wording) | **CLOSED** | "unlink-then-install — redefinition-as-replacement rides `graft_method`'s own verb" (:283-284). |
| m-3 (Makefile wiring) | **CLOSED** | `code_path.cpp` and `expand.cpp` each join `VAGENT_FRONTEND_SRCS` in phase 1 where the files are created (:83-84, :106, :114-115); explicit-list cite `Makefile:688-690` verified. |
| m-4 (fragment table producer) | **CLOSED** | Phase 1's skeleton expand emits the table; `check(program, table)` "lands *with its producer*" (:108-118). Improved past the r1 suggestion per disposition I3. |
| n-1 (stale v1.18 cite) | **CLOSED** | :3 now v1.19. |
| n-2 (limitation in Exit) | **CLOSED** | Anonymous-handler note now lives only in out-of-scope (:429). |

## The three directed questions

**Are the Requires/kernel-simulable vs adapter-gated markings honest per phase?**
Mostly yes — verified per phase: 0-1 "today's tree, no kernel" (correct — `vagent`
links `VAGENT_FRONTEND_OBJS` only, `Makefile:1147`); 2 honestly notes `static` needs a
kernel-linked harness the binary can't provide; 3 splits `run`/resolvers/`reify`
engine-level (kernel-simulable, ScalarHarness verified at
`tests/xuzz/vagent_compiler_test.cpp:18-46`) from linked-prelude execution and real
`program()`/`agent_cell()` binds (adapter-gated); 4 gates only §6.4's under-execution
arm (actor turns — correctly, they don't exist); 5 declares the whole exit
kernel-simulable through a test-owned driver on a local `Store` (sound — every object
is local); 6 "fully adapter-gated"; 7 gated on iterations 4-5+. Three honesty nits in
m-5 below.

**Does the minimal call convention collide with owed §11.1?** No — it is honestly
scoped *and* spec-conformant in shape. The spec already pins the convention: `d.vars`/
`d.values` name-keyed frame binding (§5.8, `vagent-agent-language.md:645-652`), the
two-cell `result`/`produced` spill (spec :263-264, :1381-1383), the `+d.entry` head
hop and `Return`-not-`Halt` tails (:259-270). The plan's "priced work" implements the
*consumer* side of spec'd pins — not a parallel ABI — and discloses the remainder
("§11.1's full frame discipline remains owed", :29-30, :226-227). No durable surface
depends on it: frames are per-activation runtime cells, and the `call`/`run` role
labels in `+d.refs` records encode spec-pinned semantics (entryOp vs head), not the
interim coverage. Two residual wording/coverage points (n-2, m-6).

**Phase-6's named sub-order** — the three rendered steps (seams → genesis wiring →
P16 ceremony) carry the pin correctly and the ordering chain is consistent on both
lineages. One internal wrinkle remains (m-4): step 2's ref-record arm "genuinely
requires P16" — i.e., step 3's machinery — under a "land in the listed sub-order"
rule.

Cites spot-verified beyond r1's set (all clean): `mesh_actor.cpp:659` `pending`,
`mesh_actor.hpp:81` `ActorMessage message` non-optional, `vortex_core.cpp:50-69` = 17
`mintNamedDimension` calls + `home`(:35)/`d.dims`(:44) pair, `vortex_vm.cpp:854-893`
Call frame mint + stack chain, `:98-100` `setOpcodeKind`, `:337`/`:343-346`/`:771-806`/
`:1269-1273`/`:1287-1294` the P15 funnel sites (spec :360-370 matches verbatim),
`compiler.cpp:160-181` `binding()`'s `.value()` render + `externalReferences_` write
(the cap$N bypass rationale is code-true — an unvalued capture cell trips the render
at :163), `:884-891` `localReferences_`/`externalReferences_` keying on `const Expr *`,
`:769-772` `dimConstant`/`findOrMintDimension`, `:500-502` the preflight
"construct is not supported" refusal, `:108-110` the choiceDepth/marks baseline check,
`arena_manifold.cpp:34-46` `dimensions()`/`dimensionNamed` surface, `:77-105`
`ensureDimension`'s untrailed `arenaDims_` write, `:274`/`:289-290` `attach()`'s
`d.store-refs`, `apps/vagent/main.cpp:46-53` the direct `Parser`+`check()` call,
`Makefile:649`/`:692`/`:698` (the vagent filter-out)/`:1147`/`:1348-1350`,
`vagent-agent-language.md:1670` P4 row verbatim, V10 "thirteen dimensions" (:224),
`tests/samples/vagent/mini_*.vagent` exists. Spec-side: :2230-2244 (comparand +
`globalKeyOf` arm set), :2165-2185 (no-interleave + three-state flag + ActorDown),
:2495-2510 (`src:`/`reify:`/`patch:` reservation + shared counter), :1850-1862
(`learn_*` session writes), :1157-1177 (member rows), :1053-1060 (bind set),
:2390-2402 (dim arm + orphan residual), :2906 (§9.1-only scalar subset) — all match.

---

## MAJOR

### M-1 — Genesis (e)'s dim-record arm is owned by no bullet, though the plan's own schema says (e) consumes it

**Plan lines 345-350 (phase 5 (e)) vs spec :2383-2402.**

What it claims: the (e) bullet delivers "**`+d.refs` rehydration — last pass,
name-record arm here** — the role-split rebind (`call`-role `int64(entryOp)` vs
`run`-role `int64(head)`) over registered heads *and* replay-installed copies'
regenerated records", and defers "the ref-record arm — per-record
`Int64(*handleTarget)` writebacks — ... to phase 6 with P16".

What the spec shows: the (e) sweep has **three** arms, not two. Its inventory is the
`+d.refs` ref-records *plus* "the `+d.recipe` `+d.name`-bearing records read for the
dim arm below" (spec :2386-2388): per ref-record `Int64(*handleTarget)` (the
P16-deferred arm); per name-record the role-split rebind (the arm the plan names);
**and** "the `+d.name`-bearing `+d.recipe` record — the ref-capture dim record or the
`dim`-field record alike — instead writes `Int64` of the `DimRef` `ensureDimension`'s
name arm re-resolves (`arena_manifold.cpp:78-105`), into the record's `+d.refFor`-
paired slot or the member cell itself respectively — `Linked` binds a `DimRef`, never
the promoted twin `*handleTarget` names" (:2395-2401). The dim arm is *not* the
ref-record arm — it needs no promote, no P16 flag, and belongs squarely in phase 5
beside (b)'s `ensureDimension` sweep and the filed rank it re-resolves off. It is
also what makes the plan's own orphan-residual sentence coherent — "re-bound off the
filed rank for a filed name" is this arm. And the plan's phase-3 schema text promises
it: "`+d.name` on a `+d.recipe` member reserved to the `dim` field's dim-name channel
— consumed by phase 4's `closed` re-verification **and phase 5's (e) rebind**"
(:193-195) — a consumer the (e) bullet never delivers.

An implementer building (e) from the bullet rebinds name-records and skips
dim-records: after a re-genesis, `+d.recipe`/`+d.refFor` records pointing at
`dim`-field channels keep stale `DimRef`s (or silently miss the re-resolution that
prevents per-checkpoint orphan accretion) — spec-nonconforming, and unasserted by any
exit line.

Suggested fix: extend the (e) bullet — "plus the **dim-record arm**: `+d.name`-
bearing `+d.recipe` members (ref-capture dim records and the `dim`-field member
alike) write `Int64(DimRef)` re-resolved by `ensureDimension`'s name arm off the
filed rank — no promote, no P16 dependency." One clause; all machinery named already
exists in the phase.

---

## MINOR

### m-1 — Named members still carry unbulleted priced sub-mechanisms

**Plan lines 207-215, 210-213; spec :305-331, :2504-2509.**

Two residues of r1 M-2.4's tail:

1. `test_of`/`body_of` are listed as read-group members, but their spec'd
   implementation is the clause-side **memo record chain** — `clause +d.code →
   +d.step` records of `{+d.values → wrapper head, +d.name → memo-kind label
   ("test"/"body"), +d.grab → fresh scalar key cell}`, fresh text label cells (the
   `-d.name` single-occupancy eviction rule), content-compared keys,
   unlink-on-mismatch, no record for label-less attach cells (spec :311-331; the
   member rows themselves say "wrapper memoized clause-side", :1157-1158). Unmemoized
   wrappers pass every listed exit while leaking a ~6-cell head per call — the exact
   cost the spec priced.
2. `reify`'s `reify:<serial>` label draw off the agent cell's `d.vars` counter —
   "the *same* counter" `patch:<n>` draws (spec :2504-2509) — is named only in phase
   4's patch context (:279-280); phase 3's `reify` bullet (the counter's other, and
   first, consumer) doesn't name it.

Both are discoverable in the member specs, hence MINOR not MAJOR — but this plan's
granularity elsewhere (record schema spelled out field-by-field) makes the omissions
conspicuous. One bullet clause each.

### m-2 — `learn_method`'s recorded name is "never an alias"; the plan calls it one

**Plan lines 278-282; spec :1850-1853.**

"the `d.patch` record append carrying the ordinal, target descriptor, **user
alias**, ... while the user `name` rides the record as an un-minted alias" — true
for the clause-installing verbs, but the spec is explicit that `learn_method`'s
`name` "is the symbol's functional lookup name, **never an alias** and never
minted" (:1851-1853). The slot is what `unlearn`'s read side matches ("latest patch
matching the recorded *target* name" — the plan's own phrasing at :281); calling it
uniformly "alias" invites a second field or a skipped write for the one verb whose
name *is* the target. Say "the recorded target name — the user alias for
clause-installing verbs, `learn_method`'s functional lookup name".

### m-3 — The minimal convention's coverage statement should name frame-bound capture classes

**Plan lines 224-227, 195-196; spec :847-895.**

"State the arg-binding coverage the convention delivers" is the right instruction,
but one coverage decision is load-bearing *inside the same phase*: spec :889-895 and
:847-854 make **frame-bound captures a refusal class** — "a fragment that captures a
`def`/plan parameter's or a `let` local's binding cell captures the cell, and frame
cells are `makeCell`-fresh per `#Call`". Which bindings the minimal convention puts
on the frame (parameters only? `let` locals too?) *defines* which capture-slot
emissions the phase-3 anchor rules refuse. The convention bullet and the
capture-slot wing-emission bullet sit side by side with no stated ordering between
them, and phase 2's exit already defers "capture-liveness and dead-frame rules" to
this phase. Add one sentence: the coverage statement must say which binding classes
are frame-bound, and it lands before the capture-slot refusal rules.

### m-4 — Phase 6's listed order still puts a P16-dependent arm ahead of P16

**Plan lines 385-387 vs 388-396.**

Step 2 ("Genesis hook wiring") ends with "plus the deferred ref-record arm of
spec-(e): per-ref-record `Int64(*handleTarget)` writebacks, the `OpHandle`→`Int64`
restore — **the arm that genuinely requires P16**". Under "land in the listed
sub-order" this instructs implementing the arm before step 3 lands the flag
machinery it consumes (the kind-guarded restore and the marked-cell state are step
3's). The dependency is disclosed — so this is sequencing sloppiness, not a hidden
inversion — but the clean statement is: step 2 wires the (a)–(e) driver; the
ref-record writeback lands *with* the flag machinery in step 3, inside the same
driver. Move the clause or mark it "after step 3".

### m-5 — Requires-marking honesty: three tightenings

**Plan lines 398-399, 393-394, 318-320, 236-243.**

1. Phase 6's "fully adapter-gated" overstates its own step (iv): implementing
   `wake(pid)` + `ActorExecution::message`→`std::optional` is a `mesh_actor` change
   with mesh-level tests (P4's own row: "touching the existing mesh executor and
   its tests") — implementable and testable on today's tree. Only its *invocation*
   from the checkpoint ceremony is adapter-gated. Mark (iv) implementable-now /
   consumed-later.
2. "`pending(pid)==1` arms the window" (:393) is a queue-depth-at-dispatch proxy
   for the spec's actual condition — "a turn whose end leaves the mailbox empty"
   (spec :2165-2169). A flagged turn that self-sends breaks the proxy. Restate as
   mailbox-empty-at-turn-end (with the empty-mailbox→P4-wake arm unchanged).
3. Emit-lineage filing — "the artifact's own image emission files the same
   vocabulary before *its* compile arena's `initGenesis`" (:318-320) — is substrate
   work (the artifact-emission pipeline this plan does not build), stated inside a
   phase-5 bullet as if it were phase-5 work. Mark it owed-substrate, like the rest
   of the Preconditions column.

Separately, phase 3's Exit could carry its Requires line's honesty down one level:
"resolver tests" run engine-level, not through a linked prelude — worth the same
parenthetical there.

---

## NIT

### n-1 — `vortex_core.cpp:14` doesn't name `initGenesis`'s call site

**Plan line 319.** The `initGenesis()` invocation is the `VortexCore` ctor body at
`vortex_core.cpp:8-9`; :14 sits inside `mintNamedDimension`. The `:29-70` half of the
cite is right. Fix to `:8-9`/`:29-70`.

### n-2 — "`Call` op + frame" flirts with the reading the spec carved out

**Plan line 224; spec :260-262.** The spec is explicit that `run` is "the
call/frame convention, **not a `Call`-opcode operand** — `Call` executes
`toInt(inputs[0])`, an entry-valued slot that cannot name a head". The plan's
immediately following "+d.entry head hop" recovers the correct semantics, but the
phrase "`Call` op" as the convention's first noun invites exactly the
operand-is-the-head misreading. Say "the `#Call` frame convention (the frame writes
of `vortex_vm.cpp:854-893`) — the operand slot stays head-valued, never a
`Call`-opcode operand".

### n-3 — "descriptor re-point" names the wrong object

**Plan line 339; spec :2826-2828.** `learn_where`/`learn_body` re-point the
*clause's* `+d.grab`/`+d.spin` edge at the fragment head — "re-points the clause's
`+d.grab` at the new test's *head*". "Descriptor" is the head's `+d.values` record,
a different object. "clause-edge re-point" or "the clause's `+d.grab`/`+d.spin`
re-point".

### n-4 — The prelude skeleton has no named home

**Plan line 206.** "a prelude-source skeleton in the `std:agent` style" — inert
text until the linked-prelude path lands, but no file location or wiring convention
is named (there is no `std:agent` precedent to copy — it doesn't exist yet either).
One clause naming where prelude sources live (and that nothing loads them yet).

### n-5 — Phase 2 leaves an explicit either/or open

**Plan lines 154-157.** "un-stamped `Identifier`s either fall back to the env walk
or refuse; pick one and say it" — flagged honestly, but it could be resolved in the
plan: env-walk fallback preserves today's public-`check()` behavior
(`checker.hpp:30`) and is the lower-risk pick.

---

## Cross-cutting note for the disposition

The v2 revision is a faithful application of the r1 rulings — every structural
defect closed, and the new machinery (the ordering invariant, the bind-set
enumeration, the transport cut) is correctly sequenced and correctly cited; I found
no new inverted dependency and no phase whose exit outruns its stated Requires. The
single MAJOR is the same *class* r1's M-2 caught — a spec'd mechanism slipping
between two enumerated arms — at smaller scope: the (e) bullet names arm one and
defers arm two of a three-arm sweep. If the disposers read the `+d.refs` bullet
title ("name-record arm here") as intending exhaustiveness at the *name-record*
level only, this could be scored MINOR; I score it MAJOR because the plan's own
schema text promises the consumption and no exit asserts it.
