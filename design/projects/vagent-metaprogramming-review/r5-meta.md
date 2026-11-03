# Round 5 critique — `design/vagent-metaprogramming.md` v1.4 — Metaprogramming Expert lane

Scope: convergence check on my r4 findings plus staging review of the v1.4 deltas (:702–750
cap$N stamp/value-bindings/frame-bound; :779–781 template CBV; :801–807 splice-only; :843–845
`static(param)`; :884–897 checker honoring; :1268–1282 sideness; :1349–1366 `closed` arm +
`run` records; :1385–1398 name-record chains; :1413–1456 P16 ceremony; :1463–1472 genesis;
:1504–1516 label parse; :1895–1901 §10.1 inventory). Load-bearing cites re-verified against
the tree this round.

## 1. Verdicts on my r4 findings

**meta-1 (`cap$N` stamp fate, MAJOR) — RESOLVED in substance, one vocabulary residual
(finding 3).** Option (b) landed, twice and consistently: §5.3 (:711–715) — "keeps a resolve
stamp naming the *captured binding itself*, so the checker resolves it straight to that
binding … the `{cap$N : type-of-name}` env records … are a diagnostics and readability aid —
not the resolution channel"; §5.6 (:894–897) — "`cap$N` nodes carry the captured binding's
own stamp and resolve straight to it, the env records `expand` inserts serving diagnostics
only"; and the expand bullet (:884–885) lists "binding stamps plus `CompilerBindings`
entries and diagnostic env records" — three channels, each named once. The r4 ambiguity
(stamp-vs-env-record, which channel resolves) is gone. Residual: §5.6 still defines the
stamp payload as "its binding's **scope id**" (:877) and "scope-id stamps" (:894) — a scope
id does not name a binding, and under (scope-id + renamed-name) lookup `cap$0` is undeclared
in the captured scope. The text only works if the stamp is a *binding identity* (decl ref,
or scope-id + original-name). One clause short of airtight — finding 3.

**meta-2 (frame-bound channel, MINOR) — RESOLVED.** :722–730 puts `def`/plan/handler
parameters *and* `let` locals in the ABI-debt class, states there is no emission-time cell,
that "the `cap$N` alias and its capture-slot edge are established per-activation by frame
binding", and refuses `cells"$cap$N"` anchors on frame-bound captures with the compile-time
reason (`compiler.cpp:475-483` — verified: `bindings_.find` at :475, `externalReferences_`
record at :490); item 3 (:748–749) cross-references the refusal. Consistent with :762–770
(orphaned-at-`Return`, last-written value) and :1348–1350 (un-learnable). Micro-nit: the
*binding-table entry* is compile-time; per-activation is the slot edge's *target* — the
sentence lumps them (finding 5).

**meta-3 (`closed` content arm) — RESOLVED.** :1351–1354 adopts the ruling verbatim:
"declared type is or *contains* `Cell`/`Code` (`any`, `Opt`, lists, records) — or whose
cell's content is a ref-box int naming a program-side cell".

**meta-4 (`:`-label parse) — RESOLVED — but see new finding 1.** :1504–1508: `code_named`/
`+d.name` match verbatim; first-`:` split scoped to qualified-name lookups (`plan_named`,
`symbol_named`, patch targets) with the "resolves as a module on `programRef`'s walk"
qualifier; user `#label`s can't spell `:`/`"`. :1514–1516 "`code_named` walks `d.code`
only", program-image twin unreachable; :1517–1520 grammar `[<module> ':'] [<Agent> '.']
<member>` stated once. All as ruled — and the `d.code`-only narrowing is exactly what breaks
meta-6's channel (below).

**meta-5 (OpHandle-stale wording) — RESOLVED in kind, NEW adjacent slip (finding 2).**
:1467–1470 now correctly says "receives the `OpHandle`-kind cells verbatim" — but then
claims they carry "**pre-promotion** targets". Verified `arena_manifold.cpp:1948-1951`:
`store.makeOpHandle(out.version, isEphemeral(target) ? real.at(target) : target, …)` — the
serialized handle names the *promoted* (store-space) target, which is precisely why
`Int64(*handleTarget)` restores correctly with no remap. "Pre-promotion" inverts it.

**meta-6 (`run`-target name-records) — PARTIALLY RESOLVED → new MAJOR (finding 1).** Both
asked-for statements landed: emitter name-records every statically-known `run` operand and
an opaque `run` target is a closedness refusal (:1363–1366); the rebind's different first
step is stated (:1392–1394). But the named channel is `code_named` — which under R4-B's own
ruling walks `d.code` only (:1514–1516) and cannot reach the program-side literal heads that
are the only possible statically-known operands (:1497–1499, :1515). The genesis rebind
provably misses.

**meta-7 (value-bindings only) — RESOLVED.** :709–711 verbatim: "The rename covers *value
bindings only*: program-member references (`def`/method/plan/goal names) stay name-resolved
at emission and carry `+d.refs` name-records".

**meta-8 (template CBV) — RESOLVED.** :779–781: non-`Code` args bind call-by-value through
fresh `let` locals at the inlined body's head; `Code` args are statically-known bindings,
never re-evaluated.

**meta-9 (duplicated paragraph) — RESOLVED.** Single merged paragraph at :957–964;
`isCellRelocationHandle` cite folded in; per-ref-cell minting and the `+d.name`
discriminator retained.

**meta-10 (micro-items) — ALL RESOLVED.** "when the fragment is statically known" (:1060);
"link-reachable *from the agent cell*" (:1278–1279); the head's edge named — "a `+d.values`
edge from its emission record (the same symbol→head convention `graft_method` uses)"
(:1263–1265); splice-only "refused in evaluated positions … never as a `let`/`var`
initializer or call argument", dead `let`s erased or refused (:801–807); `static(param)`
emission error (:843–845); `def run`/`def value_of` refusal under `import std:meta`
(:612–613); `lexer.cpp:14-24` (:706 — verified, `identifierPart` closes at :24); appendix
now 1.2 (:2209) → 1.3 (:2237) → 1.4 (:2270).

Also verified in passing, all consistent: `graft_handler` bound-event formals (:1118–1121);
impure-kind set in the §6.1 binding list (:925–927) and §7.2 use (:1573); `value_of` in the
pure half (:994); `run_checked` → U-M10 (:1098, :2173); genesis `opcodeMap_` step (c,
:1459–1461); `pending(pid)==1` + P4 wake + two-phase flag + kind-guarded restore
(:1427–1452); `birthOp`-bounded mint window with cites (`arena_manifold.cpp:1183-1186`,
`mesh_value.cpp:399` — both verified); `exportSymbol` `+d.values → entryOp` direct
(`vortex_core.cpp:119-127` — verified); `lexer.cpp:333-335` `$` error and
`checker.cpp:30-36` `snake` `$`-acceptance (both verified); §8.5 qualified `graft_plan`
(:1713); `frontend.cpp`/`ast.hpp` in §10.1 (:1895–1901, :1933–1935); R4-H decl repairs
(:1779–1781, :1812–1816, :1847–1848).

## 2. New findings (v1.4 deltas only)

### 1. MAJOR — §6.6: the `run`-target name-record rebinds through `code_named`, which cannot reach its target — every learned fragment containing `run` of a literal dies at genesis

:1392–1394 — "a `run` target's name-record rebinds differently at the first step only: its
`+d.name` is the target head's *label*, resolved through `code_named` → head `+d.entry` →
`writeValue(slot)`." But `code_named` "walks `d.code` only" (:1514–1516), and the only
statically-known `run` operands — literals, `let`-bound literals, `Code` parameters
(:666–670) — are *program-side* heads, which are "never `d.code`-registered" (:1515;
registration is only `learn_*`'s fresh reify, :1497–1499). The ruling text even names the
reason in the next sentence: "the program-image twin … is unreachable by construction" —
and the program-image head is precisely the run target. So `code_named(label)` returns empty
at genesis for every such record → "an unresolved name faults/drops" (:1394–1395). Worse
than a refusal: `learn` *accepted* the fragment (the name-record satisfies `closed`), and a
missed rebind leaves the slot's checkpointed `Int64` stale — in range, it silently aliases a
wrong cell (§4.5's own hazard). The two adopted rulings collide: R4-B narrowed `code_named`
to `d.code` to kill the twin-match; meta-6's fix then routed run-target rebinding through
it. Member calls survive (`symbol_named` → `+d.values` → head/op hop, :1386–1391 — that
channel is fine); fragment calls do not.

**Fix:** the `+d.name` label on a `run`-target name-record resolves against the *program
image*, not `d.code` — name a program-image label→head resolution (a `program()`-rooted
verbatim `+d.name` match over the image's fragment heads, or an adapter-bound label index),
distinct from the user-facing `code_named`. §6.7 already heads this way: "labels are the
re-resolution key" — the *lookup space* differs by head class (`d.code` for learned,
program image for literal-born); say so once, at :1392 and :1511.

### 2. MINOR — §6.6 genesis (e): "handles carrying pre-promotion targets" inverts the serialized `OpHandle`'s content

:1468–1470. The stored handle's target is `isEphemeral(target) ? real.at(target) : target`
(`arena_manifold.cpp:1948-1951`, re-verified) — already the promoted store-space id; that is
what makes `writeValue(slot, Int64(*handleTarget))` correct with no remap (`real` exists
only during publish). As written, the parenthetical implies genesis needs a translation step
the doc never supplies. **Fix:** "handles carrying their promoted targets" — matching the
disposition wording ("marked cells arrive `OpHandle`-kind with promoted targets").

### 3. MINOR — §5.3/§5.6: the `cap$N` stamp is "the captured binding's own stamp", but the stamp is defined as a *scope id*

The semantic ruling landed (meta-1 above) — but "its binding's scope id" (:877) and
"scope-id stamps" (:894) cannot literally "name the captured binding itself" (:711): a scope
id identifies a scope, and resolving `cap$0` by (scope-id, name) finds no `cap$0` declared
there. The mechanism only works if the stamp is a *binding identity* (declaration
reference, or scope-id + declared name) — which also sharpens what "does not re-walk env"
means for ordinary identifiers. **Fix:** one clause at :877/:894 — "the stamp records the
identifier's resolved *binding* (declaration identity — scope plus declared name); a renamed
`cap$N`'s stamp names the captured declaration directly."

### 4. MINOR — §6.6 sideness: walk direction unstated; live frame-bound captures can classify state-side

:1278–1279 names the root ("link-reachable *from the agent cell*") but not the direction —
while :1254–1255 says `promote` follows links in both directions, which forces the same for
the soundness-relevant walk. Under that reading the agent reaches `home`'s `+d.cursors`
rank → `cursor +d.stack` → the *live* frame chain (`vortex_vm.cpp:157-160`, `:860-890`), so
during the capturing activation a parameter/`let` binding cell is agent-reachable and
runtime-minted — i.e., **state-side** — contradicting :1348–1350's unconditional "Capturing
a `let` local or a parameter makes a fragment un-learnable — those cells are … neither
state-side nor name-recordable": `learn` invoked inside the captured `def`'s own activation
would accept the per-activation cell as durable. **Fix:** "the walk follows links in both
directions, as `promote` does — which is also why a state cell edging into program structure
classifies program-side — *excluding* `+d.cursors`/`+d.stack` runtime cells", or state that
frame-bound captures are refused by binding class before the sideness test.

### 5. Micro-nits

- :727–729 — "the `cap$N` alias … established per-activation": the `externalReferences_`
  entry is compile-time; per-activation is the capture-slot edge's *target*. Tighten to
  "the alias resolves to the emission-minted capture slot; its edge is bound per
  activation".
- :713–714 — the `{cap$N : type-of-name}` env record carries a *type*, while its stated
  purpose (render `cap$N` as the captured *name*) is served by the stamp → declaration;
  harmless, but the record's payload and purpose mismatch.
- :1464 — genesis (e) summarizes name-record rebind as "the `symbol_named → …` chain above";
  the `code_named` arm is also "above" — mooted by finding 1 either way.

## 3. Answers to the posed question on meta-1

Does "the renamed `cap$N` keeps a stamp naming the captured binding" coexist with §5.6's
env-record language — unambiguous? **At the channel level, yes**: both sides now say the
stamp resolves and env records are diagnostics-only (:713–715, :896–897), and §5.6's expand
bullet enumerates the three channels without overlap (:884–885). **At the payload level, one
clause short**: "scope-id stamp" (:877) cannot name a binding — the stamp must carry binding
identity for `cap$0` to resolve "straight to that binding" (finding 3).

## 4. Staging discipline summary

Expansion ordering is now fully consistent: resolve stamps → `expand` renames value-binding
captures to `cap$N` (binding-stamped, unlexable, one counter per run) → checker honors
stamps once (no env re-walk) → kernel lowers. The one unresolved pipeline contract is
which *rank* each durable name lives on: member calls take `d.vars`/`symbol_named`, learned
heads take `d.code`/`code_named`, and literal `run` targets currently name a channel
(`code_named`) scoped away from them (finding 1). Everything else is wording precision.

VERDICT: NOT CONVERGED — 1 major (run-target rebind channel), 4 minors.
