# r2-writer — freeze-pass critique of `design/vagent-actor-executor.md` (v0.2)

**Verdict:** all five of my r1 MAJORs landed and the corrected mechanisms are internally coherent —
but v0.2's rewritten §3.2 introduced one **new MAJOR**: the turn skeleton's step-0 ordering, commit
assert, and failure bullet cannot be satisfied by the VM primitives they name. Counts: **1 MAJOR, 13
MINOR, 5 NIT** (19 findings). mdformat `--check --wrap 100` and `mdl -i` run clean on v0.2.

## Disposition check — r1 findings vs v0.2

| r1 #                     | status                                                                                                                                             |
| ------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| MAJOR weight channel     | **Landed.** §2.1 bullet rewritten to keyed `d.vars` records (:80-85); §3.4 repriced O(V+C) snapshot (:234-237); cites now cover both arms.         |
| MAJOR §3.2 missing steps | **Mostly landed.** Step 0 (base CP/Dewey/scope) and step-9 scope-unlink/`compact_every` added. But the rewrite broke mark discipline — finding 1.  |
| MAJOR §3.1 enforcement   | **Landed.** `resolveDimension` named as the gate, `ensureDimension` disclaimer added, `arenaDims_` untrailed rationale correct (:111-117).         |
| MAJOR milestone owner    | **Landed.** M2 owns segmentation+ABI; M3 adopted-only; M4 owns resume guards; §6/§8/§3.8 aligned.                                                  |
| MAJOR `delegator`        | **Landed** (:248).                                                                                                                                 |
| MINORs (6,7,9)           | meta.hpp:196-614 ✓; `vagent.max_live_cells` + owed-not-cited ✓. **Not folded** despite "all folded": wrong §4.x cites, provenance triple, 11-vs-7. |
| MINORs/NITs (rest)       | Several remain — see findings 5-18.                                                                                                                |

## The eight checks

1. **`+d.dirs/+d.agents` — CONSISTENT.** §3.1 diagram (:120), §3.1 bullet (:143-153), §6 row (:430),
   §9 decision 3 (:493-496), §10 M0 (:515), §8 M0 (:454) all agree. `home +d.agents` and
   `cells"##/+d.agents"` survive only as the explicitly-refuted naive form. The promote story is
   coherent: dirCell carries only membership ranks → its expansion commits only the intended diff;
   home's one `+d.dirs` edge is a single Additive commit (residual `+d.cursors` accretion is
   honestly filed as §5.2, pre-existing).
1. **Step numbering — CONSISTENT in form, BROKEN in content.** "steps 0–9" (§8 M1, :464), "step-0
   base CP" and "inside 1–6" (:191) all match the new 0–9 list. But see finding 1: the ordering the
   list encodes contradicts the spec contract it cites and its own step-7 assert.
1. **Weight-snapshot — CONSISTENT.** §3.4 O(V) snapshot + O(C) walk + absent→0.0 + rank-position
   tie-break; §6 "same greedy semantics + early exit; rank-position tie-break; no maintained
   invariant"; §9 d1 "weights read from the `d.vars` record channel". §2.1 and M3's "weight-snapshot
   \+ virtual order" wording agree.
1. **Registered-head-name — CONSISTENT.** §3.5 records the `+d.spin` target's registered-head name
   (`patch:<n>` verbatim re-mints — genesis.hpp:30-35 confirms re-mint stability while
   `reify:<src>:<serial>` re-draws; `codeNamed` channel; declared names for image-originals). §9 d2,
   M0 (:452), M3 (:476), M4 (:479) form a clean emit→record→check chain. Rehydration-safe.
1. **§5.x numbering — CONSISTENT.** §5.1 backtrack / §5.2 cursor accretion / §5.3 alloc profile; all
   inbound refs (:9,:193,:376,:404,:414,:463,:518) point at the right defects. No stale "§5.2 =
   step() profile" references.
1. **New cites — mostly accurate, two wrong.** Verified: genesis.hpp:30-33 (reify serial re-draw,
   "(each install copy's own `reify:<src>:<serial>` still draws)"); meta.cpp:34-38,:967 (d.clone
   lazy mint comment + `resolveDimension(kCloneDimName)` site); agent-language:341-343 (exact
   commit/undo idiom); meta.cpp:818-825 (exact mark refusal); arena_manifold.cpp:78-105
   (`ensureDimension`, no mark check — confirmed); arena_manifold.cpp:1794-1796 (pos+neg discover,
   bidirectional); vortex_vm.cpp:242-245 (`+d.cursors` tail append); mesh_actor.hpp:55-65 (no
   `maxLiveCells`); records.hpp:69-79 (linear `rankAfter` scan); meta.cpp:602-619/:2038-2064
   (weightOf/setWeight both arms); metaprogramming §6.4 (:1486-1518 drain idiom — exact) and §6.6
   (:1806 — the agent→home link-path invariant, accurately invoked); §9.2's
   `code"believes threat(?t) & t > 0.9"` (:2825). **Wrong:** metaprogramming "§9.1's clause-walk
   lowering" (finding 3); meta.cpp:1850-1851 still learn_body only (finding 4).
1. **Format — CLEAN.** mdformat `--check --wrap 100`: no diffs. mdl `-i`: no violations. Heading
   levels and table structure regular.
1. **New contradictions — ONE, central.** §4.2's "ActorExecution gains only the arbitration counters
   (§4.7)" and §4.7's "replay record: … + arbitration counters" agree (counters flow through
   ActorExecution into the record; M0.5 names both). The contradiction is elsewhere — §3.2's mark
   discipline, finding 1.

## Findings

1. **MAJOR — §3.2's rewritten base-CP/commit/failure text is mechanically inconsistent with the VM
   primitives it names and the spec §4.2 order it cites.** Three defects, all in v0.2-new text: (a)
   **Step 0 puts the base CP *before* `m0`; the spec puts it after.** Spec §4.2 is Mark → Fuel →
   Turn-cell → **Base** (`vagent-agent-language.md:323-336`): `pushChoicePoint` always takes
   `arena.mark()` (`vortex_vm.cpp:301`), and "marks are assumed well nested: release() and discard()
   pop" (`arena_manifold.hpp:843`) — `discard(m)` ignores the mark argument and pops `floors_` LIFO
   (`arena_manifold.cpp:1596-1610`, `:1697`). With m_base below m0, the base CP's `discard` pops
   *m0's* floor — the named-mark/floor correspondence scrambles (counts still net out) — while
   buying nothing: `release()` never touches `choiceStack_`, so the base CP "survives" `release(m0)`
   regardless of push order. (b) **Step 7's assert is unsatisfiable.** "commit: discard(m0) → …
   assert choiceDepth==0 && marks==0" omits the spec's explicit first half — "On success:
   `clearChoicePoints()`, then `discard(m0)`" (:340). The step-0 base CP has no `#Cut`; it exists
   precisely to persist through the turn ("makes 'no remaining choice points' unreachable", :360),
   so `choiceDepth` is ≥1 at the assert. M1's skeleton inherits the gap ("steps 0–9 …
   commit/**clearChoicePoints→release**" names the clear only on the failure arm, :467). (c)
   **"fail_step runs against the surviving base CP" contradicts the primitive in the same
   sentence.** `clearChoicePoints()` = `cut(0)` — it pops the *entire* stack, base CP included, and
   **discards** each CP mark (`vortex_vm.cpp:337-345,:360`), neither draining "to" the base CP nor
   "releasing" marks as the parenthetical claims. The spec's recovery is "a short turn of
   `fail_step`" run *after* clear+release (:341-343,:363) — the base CP is consumed during the turn
   (backtrack lands on its step-failure alternative) or cleared with everything else; nothing
   "survives". **Fix:** restore the spec order (mark → fuel → `$turn` → base), or explicitly design
   and price the base-before-m0 deviation; write "clearChoicePoints() → discard(m0)" on the success
   arm; and either describe fail_step as the spec's post-release short turn or say the drain stops
   at depth 1 (`cut(1)`) and backtrack fires the base alternative — the current sentence names
   `clearChoicePoints()` and cannot have a surviving CP. Borders on BLOCKING (the normative skeleton
   M1 implements cannot execute as written); rated MAJOR because the correct recipe is verbatim in
   the spec the same bullet cites.
1. **MINOR — `d.verb` is filed nowhere.** §3.8's own gate: "every dim a verb might need … is filed
   at genesis" (:311-312) — yet the new ABI's `+d.verb → <verb-name>` (:316) is absent from M0's
   enumeration (`d.agents`/`d.dirs`/`d.seen`/`d.evidence`/`d.clone`/recipe, :454-455). A mid-turn
   `learn_*` installing a body that contains host-step segments (the §9.3 learned plan calling
   `set_weight` *is* such a body) would emit the `+d.verb` descriptor under `m0` and hit the
   `resolveDimension` refusal the doc describes. **Fix:** add `d.verb` to the M0 filing list.
1. **MINOR — new miscite: the clause-walk lowering is agent-language §9.1, not metaprogramming
   §9.1.** §3.8's fragment-position bullet cites "`vagent-metaprogramming.md` §9.1's clause-walk
   lowering" (:301) — metaprogramming §9.1 (:2790) is the `splice`/`code` example and contains no
   clause-walk lowering. The prescribed lowering is **`vagent-agent-language.md` §9.1 "Cross-walk"**
   (:1479): "`believes t` | clause walk with `#Unify`, one `#Choice` per clause" (:1500).
1. **MINOR — graft_body cite still incomplete, and "named head" overstates.** §3.5 attributes
   "`graft_body`/`learn_body` install a fresh named head" to `meta.cpp:1850-1851` (:263) — that
   range is `learn_body`'s repoint only; `graft_body` repoints at `meta.cpp:1345-1346` (my r1 fix
   asked for both). Worse, `graftBody` installs via `installable()` with **no `registerHead`**
   (:1343-1347) — the copy is `reify:`-labeled, not registered-named. The fault outcome is
   unaffected (absent name ≠ recorded name → `body_replaced`), but the sentence overstates the
   mechanism. **Fix:** cite both sites and say "fresh head" (named for `learn_body`, reify-labeled
   for `graft_body` — either way it fails the name check).
1. **MINOR (residual r1) — §3.2's §4.x pointers still wrong.** Step 5's "aggregate condition-fuel
   account (§4.3)" (:180) and the failure bullet's "reserved fuel (§4.3)" (:197) both point at §4.3
   (the deferral/exhaustion rule) while the account and the reserve are defined in §4.2 bullets 1
   and 3 — the doc's own :229 gets the same section right for the `condition` fault. Step 2's
   "(coalesced, §4.1)" (:175) points at arbitration when the coalescing rule is §3.3's wake
   paragraph. **Fix:** §4.2 ×2, §3.3 ×1.
1. **MINOR (residual r1) — "provenance triple (source/ordinal/requestId)" (:208) still contradicts
   §3.2's field list.** The event cell's implicit fields are "source/space/time/ordinal" (:174);
   `requestId` is the wire key, not a provenance field. The doc never defines the term.
1. **MINOR (residual r1) — question arithmetic still unreconciled.** Header: "eleven spec-level
   questions" (:16); §7 codebase row: "enumerated 7 owner-level questions → 3 asked" (:445). 11 vs 7
   still unexplained.
1. **MINOR (residual r1, partially landed) — bare `§9.2`/`§9.3` self-collision persists.** §1 (:55)
   now qualifies with the file:line cite — good — but the referent remains ambiguous at
   :160,:241,:266,:298,:431,:480,:494,:529. Worst is :266 "intended per §9.2": this document has a
   §9 whose item 2 is `body_replaced` — exactly the wrong referent. **Fix:** qualify all as
   "metaprogramming §9.2/§9.3".
1. **MINOR (residual r1) — §4.1's policy is still not self-contained.** `msgStreak` is declared and
   never used in the rule (:330-332); `B_msg(p)`/`B_wake(q)` still don't say what `p`/`q` count;
   "else message and reset" still doesn't say which counter resets.
1. **MINOR (residual r1) — §3.4 still quotes a paraphrase.** "Spec §6.3's 'first applicable in rank
   order'" (:241) — the spec's words are "the first plan in the plan list" (:758) / "first
   applicable untried" (:2857). De-quote or quote verbatim.
1. **MINOR (residual r1) — §9's U-A8 label still wrong.** "turn journal to `system://activity`
   (U-A8)" (:511): spec U-A8 (:3045-3074) is deterministic *replay recording* — §4.7's item. An
   activity-store journal is a different deferral.
1. **MINOR (residual r1) — "Forced rulings (no real alternative)" still overstates items 4 and 6**
   (:498-506): one-wake-per-item and a distinct idempotence-key shape were real alternatives priced
   out — that is good dialectic, not force. Rename the class.
1. **MINOR (residual r1) — §3.7 still never states the drain order.** Spec §4.2 prescribes a kind
   order (:348-350: local spawns → messages → effect cancels → `preempts` effects → other effects →
   timer changes); the doc's `requestId` triple keys on "outbox position" (:288), leaving positional
   vs kind-order dispatch unspecified — the load-bearing half of my r1 finding's fix that did not
   land.
1. **MINOR — the `vagent.max_live_cells` → `grant.maxLiveCells` pipeline is unstated.** §4.6's
   invariant is bound to a `vagent.*` setting while the enforcement field lives on the grant; §4.2
   says `ActorOptions`/grant never carry language policy — so nobody is named as copying the setting
   into the grant at admission. One line (M0.5, at grant construction) closes it.
1. **NIT — §3.1 `[fact]` subtree glyphs.** All three children render `└─` (:125-129); the first two
   must be `├─` (the v0.1 two-child version had the same bug; the `+d.code`/`+d.patch` packing was
   fixed).
1. **NIT — drain/scope ordering deviates from spec §4.2 silently.** Spec runs Scope(8) → Drain(9) →
   Derive(10); v0.2 runs drain(8) → scope-unlink+derive(9). Post-commit C++ either way — immaterial,
   but worth a "(order swapped; post-commit reads commute)" note in a spec that diffs everything
   else.
1. **NIT — `mesh_actor.cpp:549-551` still cites only the catch mapping** (:200); the "actor stops
   forever" mechanism is `:612-617`.
1. **NIT — miscellany.** M0 still "(unowned prerequisite)" with no owner named (:449); bare
   `belief_event_depth` at :179 vs the spec's `vagent.belief_event_depth`; related-docs still
   "§6.5–6.6 intentions" (:21 — intentions are §5.6; §6.5/§6.6 are deliberation/suspension);
   "`weightOf`/ `setWeight`" stray space (:78); M0's DeclKind table says "18 DeclKinds" — the cited
   `ast.hpp:185-230` range still reads as the Decl block, fine, but the count itself is uncited.

## Verified-accurate in v0.2 (no action)

`resolveDimension` refusal text and "the mint arm is `ensureDimension`" comment (meta.cpp:812-825);
`ensureDimension`'s mark-free mint + `d.dims` rank filing (arena_manifold.cpp:78-105); `arenaDims_`
as `std::unordered_map` untrailed for rollback (arena_manifold.hpp:826 — the `:2152-2166`
prefix-snapshot copy is checkpoint export, not mark trail; the claim holds); promote's
`discover(edge.pos/neg, true)` bidirectionality (:1794-1796); `spawnCursor`'s `+d.cursors` tail link
(vortex_vm.cpp:242-245); `ActorGrant` fields (mesh_actor.hpp:56-66); `varsRecord` linear scan
(records.hpp:69-79); `weightOf`/`setWeight` two-channel behavior (meta.cpp:602-619,:2038-2064);
`pushChoicePoint`'s unconditional mark (vortex_vm.cpp:301); `clearChoicePoints()==cut(0)` and
`cut`'s `discard(cp.mark)` (:337-360); `ChoicePoint.mark` member (vortex_vm.hpp:98); spec §4.2
ten-step list and recovery wording (vagent-agent-language.md:323-365); metaprogramming §6.4 drain
idiom (:1486-1518), §6.6 link-path invariant (:1806), §9.2 `believes threat` payload (:2825);
agent-language §5.6 record incl. `delegator` (:596-615) and §9.1 lowering table (:1479-1500).

VERDICT: NOT CONVERGED
