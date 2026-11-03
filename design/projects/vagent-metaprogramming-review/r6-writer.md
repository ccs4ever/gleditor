# r6-writer — convergence critique of `design/vagent-metaprogramming.md` v1.5

Scope: per-finding verdicts on r5-writer's R5-1..R5-5 against v1.5 (2,397 lines), new-defect
sweep of the ~100-line delta (v1.4 `f7229f4f` → v1.5 `96ed8103`), `mdl -i`, verified against
`vagent-agent-language.md` v3.9.

## Verdicts on R5 findings

| R5 | Verdict |
| --- | --- |
| R5-1 MAJOR `symbol_named` owner domain | **Fixed.** §9.3 :1908 now `symbol_named("demo", "Feed")` — module owner, matching spec §5.7 (:627-634: one name cell per goal/event on the module's `d.vars`, members as `Agent.def`); the `demo` module name is consistent with §9.2's `demo:Guard.cautious`. Table row :1000 "a `d.vars` symbol cell on the named module … `owner` is the module only"; §6.7 :1589-1596 "owner is the module *only* … there is no agent-owner arm"; §6.3 rewritten — goal/event :1127-1130 (module as owner), `graft_method` :1148-1152 (`owner` = `Agent` qualifier resolving the module, symbol minted under `Agent.member` on that module's `d.vars`); genesis rebind :1432-1433 applies "§6.7's owner rule". No agent-owner wording remains. Changelog :2387-2388 claims match the delta. |
| R5-2 MINOR `opcode_named` dangling | **Fixed.** :1523-1524 reads `getOpcodeKind` (`vortex_vm.cpp:102-114` — verified: the function spans :102-114, and its `opcodeMap_` arm is :103-105, matching the :1235 cite). `opcode_named` survives only as the changelog's rename note :2393. Consistent with the doc's four other `getOpcodeKind` uses. |
| R5-3 MINOR `reify` spelling | **Fixed.** :1571-1572 `reify:<src>:<serial>` with `(<src>` is the reified head's own label, `<serial>` the duplicate disambiguator) — matches :1050 and the changelog; no bare `reify:<serial>` remains. |
| R5-4 MINOR `d.extends` omitted from walk | **Fixed.** :1292 adds `d.extends` to the structural vocabulary; the claim is re-scoped to "every dim the program skeleton hangs on" with `d.dims` the one named exclusion :1294-1296 — `promote` skipping `d.dims` verified at `arena_manifold.cpp:1766`. |
| R5-5 nit bundle | **Fixed (all six).** (a) :1573-1574 now "(the `'#' Ident` grammar, §5.1)" — `CodeBody ::= [ '#' Ident ]` at :634; (b) `learn_*` row :1019 names the registered-head-arg carve-out; (c) checker comment :1986 has a single "run(c, [args]) special form + value_of" — the duplicate is gone; (d) `next_rand` moved into the pure block above `run` (:1010 vs :1011); (e) :1244 `exportSymbol`; (f) :1386-1387 "or, on a `run`-target record, the target head's label". |

**All r5 findings resolved.**

## Citation spot-checks on the v1.5 delta (all verified)

`vortex_vm.cpp:102-114` `getOpcodeKind`; `arena_manifold.cpp:1766` the `d.dims` skip;
`arena_manifold.cpp:1779-1780` `discover(edge.pos, true); discover(edge.neg, true)` — promotes
walks both directions, so "in either direction, as `promote` walks" (:1309) is literal;
`arena_manifold.cpp:1489-1490` shadow `birthOp`/base-ref comment; `arena_manifold.cpp:1949`
`real.at(target)` in the `OpHandle` promote arm; `mesh_value.cpp:485-501` unmarked `OpHandle`→
`None` demotion (:485-490) and marked→local-twin `OpHandle` re-mint (:494-500); `ops.hpp:154-163`
the `ValueKind` enum (`OpHandle` :158, `ForeignOpHandle` :161); `arena_manifold.hpp:467-469`
`refOf = ephemeralBit|dense`; `cell_views.hpp:257-260` `rankTail`; `vagent-agent-language.md:1670`
lands exactly on the P4 row (Local wake, approved); `mesh_actor.cpp:659-662` `pending`→
`mailbox.size()`, `:462` empty-mailbox skip, `:672` `checkpointView` non-empty refusal, `:473-477`
`executor_` callback; `mesh_evaluator.cpp:735` per-reference marker mint.

## New findings

**R6-1 MINOR — `patch:` in the head-label rebind set has no referent.** :1438-1439: "A *label* —
`src:`, `reify:`, `patch:` or a user `#label`, which is what `run`-target and fragment-identity
records carry — rebinds through the **program-image label→head lookup** … matching each fragment
head's `+d.name` verbatim". But the head-label inventory at :1570-1574 enumerates exactly three
kinds — `src:`, `reify:`, user `#label` (colon-free) — and `patch:<n>` is defined only as the
learned-*clause* name convention (:1060-1061, carried on the clause cell's `d.name`, §9.3 :1900).
No stated mechanism labels a *head* `patch:`, so a `patch:`-shaped record can never match a head's
`+d.name` — the arm is dead unless learned heads are meant to take `patch:` labels, which the doc
nowhere says. The same list recurs in the P16 register row :2030 and changelog :2384; related
residue: :1577's "`patch:near_water` is just a label" to `code_named` already presumes `patch:`
labels exist in the `d.code` space.

**R6-2 MINOR — §6.7's unified grammar bullet misdescribes `plan_named`'s domain.** :1589-1596
says "the remaining dotted prefix is part of the stored symbol name itself: all symbols live on
the resolved module's `d.vars`". For `symbol_named` that is correct; for `plan_named` the stored
name is the *clause's* `+d.name` qualified string (:1708 "the clause's qualified `d.name` string",
:1372-1374), not a `d.vars` symbol — a plan is a clause on the goal's `d.clause` rank, never a
symbol. Only the closing "walks `d.name`/`d.vars` labels verbatim" loosely covers the clause arm,
and the bullet never says which part applies to which function. Second-layer nit: the
`[<module> ':']` grammar describes a single qualified string while `symbol_named` takes two args —
"the first `:` separates the module qualifier — `owner` is the module *only*" conflates the string
grammar with the two-parameter signature.

**R6-3 question/nit — `home`'s ranks named program-side anchors.** :1313-1314 lists `home`'s
`+d.cursors`/`+d.stack` ranks among the "program-side anchors" the transit clause prunes behind
(changelog :2382 repeats it). That holds only if `program()` binds the program-image root at
`home` level — plausible, since `d.stdlib` in the walk vocabulary is a home-rank dim
(`vortex_core.hpp:51`) — but the doc never states `program()`'s binding level, and in a
fresh-incarnation arena `home` is arena-minted (ephemeral ref → fails the persistent arm). One
clause naming what `program()` binds would close it; flagged for the vortex lane to confirm
rather than counted as a defect.

**Nit bundle:** (a) §13 :2165 "the argument itself left dead" vs :1289 "the argument head is left
untouched" — the argument remains live session data; "left dead" misleads. (b) Glossary `closed`
:137 still says name-records carry "a qualified name" — the `run`-target label arm added at
:1386-1387 never reached the glossary row. (c) `manifold.hpp:154` cite lands on the blank line
between `ephemeralBit` (:153) and `isEphemeral` (:155-157) — should be `:153-157`. (d) :1409-1410
"a `code_named`/`reflect`-born operand → its `d.name` label" presumes reflect wrappers carry
labels, but the label inventory names only three head-label kinds — the wrapper-label convention
is unstated (folds into R6-1's gap).

## Mechanical checks

`mdl -i` on the file: **clean, exit 0**. 22 fence markers = 11 tagged pairs. All 16 TOC anchors
resolve; heading numbering sequential (§1–§15 + appendix). All table groups have consistent pipe
counts. No `CellImport`/`MeshOp`/`opcode_named`/`symbolNamed`/bare-`reify:<serial>` remnants
outside changelog history.

**Changelog 1.5 accuracy:** every claim verified against `git diff f7229f4f 96ed8103` — the only
shorthand is :2378-2379 "ephemeral and post-extent refs stay state-side" (they classify
non-program-side; state-side additionally needs agent-cell reachability) — acceptable in a
changelog.

VERDICT: NOT CONVERGED (minor level) — all r5 findings resolved; 2 new minors + nit bundle in the
v1.5 deltas. No majors; the defect surface continues to contract.
