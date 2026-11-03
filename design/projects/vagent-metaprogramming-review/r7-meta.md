# Round 7 critique — `design/vagent-metaprogramming.md` v1.6 — Metaprogramming Expert lane

Scope: verdicts on my r6 findings against the v1.6 deltas (:1442–1503 writeback split +
divergence clause, :1574–1605 image-first genesis, :1635–1660 clone ordinals + first-`:` rule,
:1341–1347 live-frame guard, :1413–1422 two-arm `closed`, :968–970 legend, :2106 P16 row),
plus staging review of the deltas only.

## 1. Verdicts on my r6 findings

**r6-major (entry-vs-head writeback) — CLOSED in its stated form; the same hazard resurfaces
one level up as a *sequencing* bug (finding 1).** The split landed exactly as ruled: :1470–1474
names the two slot conventions outright — a `Call`-operand slot is entry-valued, a
`run`-target/`Code`-valued operand slot is head-valued "because `run` derefs the head's
`+d.entry` *and* `+d.values` at run time (§6.2), so an op cell written there would kill the
fragment on first `run`" — and the arms now write different payloads: the qualified arm ends
`writeValue(slot, int64(entryOp))` after the op-or-head hop (:1483–1485), the label arm ends
`writeValue(slot, int64(head))` — "the head itself, no `+d.entry` hop, because the slot is
head-valued" (:1488–1491) — with `code_named`'s `d.code` walk the stated fallback for
`learn`-registered heads (:1491–1493). §6.2 now asserts the convention at the lowering
(:1084–1088: "the operand slot is **head-valued by construction**: the emitter writes
`int64(head)` into it"), the legend (:968–970) and the P16 row (:2106) carry the same split,
and "fragment-identity" is defined as the Code-valued operand class (:1473, :1487). The R6-B
payload bug cannot be written by either arm *as emitted*: head-valued records carry labels
only — `run` operands are recorded only when statically known (literal → `src:`,
`code_named`-resolved → `d.name` label; opaque/`reflect`-born → refusal, :1445–1450) — so no
qualified name ever rides a head-valued slot. What remains is *when* the qualified arm
resolves: finding 1.

**r6 minor 2 (`reflect`-born arm) — RESOLVED.** :1448–1450: "a `reflect`-born operand is a
session-side wrapper with no durable referent, so an opaque or `reflect`-born `run` target in
a learnable fragment is a closedness refusal at emission" — verbatim the ruling.

**r6 minor 3 (module-name hijack) — RESOLVED in mechanism, one consistency gap (finding 2).**
Both halves of the ruling landed: the shape test "splits on the first `:` *and requires the
first segment to resolve as a module* on `programRef`'s walk, else the whole string is a
verbatim label" (:1475–1477), mirrored for `plan_named`/`symbol_named`/patch-target names with
`code_named` exempt (:1644–1647). `patch:` is out of the rebind shapes (:1487 lists
`src:`/`reify:`/`#label` only — the clause-name convention ruling applied), and :1648–1649
declares the automatic prefixes label-reserved. The residual is enforcement, not mechanism —
finding 2.

**r6 minor 4 (`home` mis-attribution) — RESOLVED, one load-bearing nit.** :1341–1347 names the
real guard, in order: `d.stack` hangs off *cursor* cells, never `home`; `home` is
"arena-minted and ephemeral, not a program-side anchor"; frames stay out because "`home`
carries no link the agent cell can reach — `##` anchors are ref-box ints, invisible to the
link walk"; binding-class refusal still sits ahead of the sideness test as the second layer.
The r6 open door (a minted structural edge making `home` agent-reachable) is closed by
assertion: the ref-box sentence covers `##` *reads* — nothing states that no verb mints an
edge *into* `home`. Nit: name the invariant ("no construct mints a `+d` edge into `home`").

**r6 minor 5 (M13 clone labels) — RESOLVED in substance, determinism unstated (nit).**
:1636–1638 gives each clone "its *expansion site's* position plus a clone ordinal,
`src:<module>:<line>:<col>+n`" — N sites mint N distinct labels, same-site clones split on
`+n`, and since emission happens post-expansion each `run`-target record stores its *own*
site's clone label: the cross-site rebind hazard is gone. Unstated: the `+n` counter's scope
and allocation order — the label is a durable re-resolution key, so a recompile that numbers
clones differently mints labels the checkpointed records cannot match (dropped patches on
artifact rebuild); and `+n`'s composition with `+o:c` for literals nested inside clones
(`src:S+n+o:c` presumably — say it).

**r6 nit bundle — applied.** Verified: legend names both record kinds *and* both payloads
(:968–970); glossary `closed` reads "field/member-var" with the both-arms refused clause
(:137); `cap$N` alias-compile-time fix landed verbatim (:743–745); `d.stack`/cursor cites
corrected (:1342–1343); `ensureDimension` find-or-mint (:1587–1588); `plan_named` matching
clause `+d.name` (:1668–1670); emission-time `#label` uniqueness (:1641–1642); the two-arm
`closed` clause (:1420–1422); extent on the `programRef` bind record (:1315–1316,
:1589–1593); image-first sequencing spelled out with the ref-preservation argument
(:1574–1584, :1318–1325).

## 2. New findings (v1.6 deltas only)

### 1. MAJOR — §6.6/§6.6-ordering: name-record rebind runs at (e), before `d.patch` replay (f) restores patch-installed bindings — calls to learned or replaced symbols bind stale or die

The genesis order is (a) image → (b) overlay → (c) dims+extent → (d) `programRef` → **(e)
`+d.refs` rehydration — "per name-record the shape-split rebind chain above" (:1595–1596)** →
**(f) `d.patch` replay** (:1604). The qualified arm resolves `symbol_named` against the
*pre-replay* image, but a patch-installed binding exists only after (f):

- **Replace case — silent misbind.** `learn_method`/`learn_where` on a program-original
  symbol is §6.6's own replace discipline (:1609–1614): the re-point lives on the
  program-side `d.vars` binding, dies with the skeleton, and is re-run only by replay at (f).
  At (e), `symbol_named` resolves the *image* binding — the original entry op — and writes it
  into the Call slot; nothing at (f) re-resolves records. The learned plan then calls the
  original method forever — exactly the R6-B failure shape (silent wrong bind), now at
  sequencing level. It contradicts two claims inside this same delta: the divergence clause's
  "a `graft_method` re-point between checkpoint and resume is picked up by genesis's rewrite"
  (:1496–1498) and the §9.3 rationale — "`rebound at genesis to whatever `navigate_to` then
  names`" (:1501–1503). A learned `navigate_to` replacement is precisely what it will *not*
  pick up.
- **New-name case — dropped patch.** A Call record naming a symbol whose only binding is
  patch-installed misses `symbol_named` entirely at (e) → "an unresolved name faults/drops
  the patch" (:1493–1494) → the caller's learn is silently un-done.

The ordering is not careless — (e) *must* precede (f), because replay reifies the
`d.code`-registered head into the installed copy, which has to inherit already-rebound int64
slots. So the fix is not a swap: either a **second name-record sweep after (f)** (ref-records
stay single-pass at (e) — their targets are state-side and replay-independent; name-records
are idempotent rewrites, so a post-replay pass is safe), or an honest scope clause —
name-records bind the *image* definition and patch-installed redefinitions are invisible to
recorded calls — which amends :1496–1503 and weakens §9.3's stated motivation.

### 2. MINOR — §6.6/§6.7: the `src:`/`reify:` reservation has no enforcement point and :1477–1479 still prices its absence

:1648–1649 declares the automatic prefixes "label-reserved, so no module can take those
names and reroute a label down the qualified path" — but no check site is named (module
declaration is compile-time; nothing says the checker refuses `module src`), and the §6.6
sentence still contemplates the un-reserved failure: "unless a module literally takes that
name (which degrades to a dropped patch, never a wrong bind)" (:1477–1479). Both cannot
stand: either name the refusal point (checker at the module decl — and the drop-not-misbind
reasoning, "symbol names never contain `:`", deserves stating) and delete the parenthetical,
or drop the reservation claim and keep the priced failure mode. Related gap: `patch:` is
*not* among the reserved prefixes (:1648 names only `src:`/`reify:`), so a module literally
named `patch` still hijacks `plan_named("patch:<n>")` lookups — the learned-clause naming
convention (:1068–1069) becomes unfindable through the resolve-as-module test (a miss →
`unlearn` cannot match it → patch leaks). Reserve `patch:` too, or state it exempt.

### 3. MINOR — §6.6: the `code_named`-resolved operand's record key is ambiguous ("its `d.name` label")

:1446–1448: "a `code_named`-resolved operand → its `d.name` label". The referent of "its"
should be fixed: the emitter records the `code_named` call's *literal label argument* —
which equals the resolved head's `d.name` only because `code_named` resolves by label. If the
argument is not a string literal, or the operand is `code_named` reached through a `let`
alias or any non-syntactic path, nothing is statically known and the operand belongs to the
opaque arm. One clause — "the operand is `code_named(L)` with `L` a string literal → record
`L`; otherwise it is opaque → refusal" — removes the ambiguity.

### 4. Micro-nits

- :1470–1479 — the shape test stands in for a recorded slot-convention bit: it is sound only
  because head-valued records never carry qualified names and Call records never carry
  labels (the emission arms are co-assigned). State the invariant — "a record's `+d.name`
  shape is a proxy for its slot's payload convention, fixed at emission; a qualified name on
  a head-valued record is malformed" — or record the convention explicitly, so a future
  record kind (e.g., a `symbol_named`-bound `Code` operand) cannot reintroduce R6-B inverted.
- :1488–1489 — the label→head walk matches "each fragment head's `+d.name` verbatim" but
  never states how the walk identifies a fragment head among all `+d.name`-carrying cells
  (clauses carry `patch:<n>` and `module:Agent.plan` labels on the same dim). Syntactic
  non-overlap makes collisions unreachable; say the match is restricted to cells in the
  head layout (`+d.entry` + descriptor `+d.values`) or that label namespaces can't overlap.
- :1636–1638 — `+n` clone ordinal: counter scope and allocation order unstated (verdicts,
  minor 5) — the label is a durable key; determinism across recompiles is required.
- :1345 — "`home` carries no link the agent cell can reach": the load-bearing invariant is
  that nothing mints a structural edge *into* `home`; the ref-box sentence covers `##`
  reads only.

## 3. Staging discipline summary

The r6 round asked for one thing at this joint — split the writeback by slot convention —
and v1.6 delivers it cleanly and consistently across all four statement sites (§6.2 :1084,
§6.1 legend :968, §6.6 :1470, P16 :2106), with the head-valued convention asserted at the
lowering itself. The shape test is now doing triple duty — lookup channel, payload
convention, reserved-prefix guard — and each duty has one unpriced edge (module-prefix
enforcement, the unstated co-emission invariant, `patch:` unreserved). The major is the
familiar pattern of this document: the newest machinery is correct per-record and wrong
across a step boundary — name-records resolve one replay-phase too early, so the divergence
clause's own example is the case it breaks.

VERDICT: NOT CONVERGED — 1 major (name-record rebind precedes patch replay: calls to
patch-installed/replaced symbols bind stale or drop), 2 minors, 4 nits.
