# Round 11 critique — `design/vagent-metaprogramming.md` v1.10 — Metaprogramming Expert lane

Scope: verify my r10 minor + nits (`cells"$a/+d.code"` off `agent_cell()`; `code_named`
head-labels-only; head-less slot observable content; emission-fixed slot genesis-only fill);
verify R10-A's `patch:<n>` minting against the staging taxonomy (alias vs `unlearn`
name-targeting vs dedupe-by-name; `learn_plan("Feed","near_water",…)` coherence); check the
three ergo-adjacent items (emission-refused `patch:` vs learn_method qualified-success;
`unlearn` LIFO same-name limit); hunt new defects in the v1.10 deltas only (full diff
`git diff 11fec218..3f9731da` reviewed). Code citations re-verified against
`apps/common/xanadu/`.

## 1. My r10 minor + nits — all landed

- **`cells"$a/+d.code"` off `agent_cell()` (:1075) — FIXED.** The row now anchors on
  `a := agent_cell()`, states *why* (`d.code` hangs on the agent cell, not arena `home`, so
  `##` reads an empty rank), and closes the `[Cell]`→`Cell` nit ("either path yields
  `[Cell]`, so the caller indexes/`for`s down to the single `Cell"`). The §6.7 echo
  (:2058-2060) inherits cleanly via the "§6.1's row names the paths" reference.
- **`code_named` head-labels-only (:1074) — FIXED.** "the space is *head labels only* —
  qualified names are `symbol_named`/`plan_named`'s space, clause labels `plan_named`'s" —
  which is exactly the partition the rest of the doc assumes: `d.code` registrations carry
  `reify:`/`src:`/`#label` head labels; `patch:<n>` clause labels live on `d.clause` rank
  cells and are "never a head label" (:1739-1740).
- **Head-less slot observable content (:1688-1690) — FIXED.** "leaves the slot **unwritten**
  (its observable content is `None`; there is no sentinel and the §6.2 descriptor deref
  fails cleanly on it)". `ValueKind::None` is the real unwritten-content kind
  (`vortex_core.cpp:224`, `vortex_vm.cpp:143`), so "no sentinel" is honest — a `None` cell
  is just a cell with no value write, and `run`'s `+d.entry` deref fails on it like any
  missing link.
- **Emission-fixed slot, genesis-only fill (:1693-1698) — FIXED, and consistent.** "an `L`
  unresolved at emission stays refused for the rest of the incarnation even when a *later*
  `learn` registers that very label — `code_named(L)` would then resolve, but no in-session
  path writes the slot; only genesis's rebind fills it." This is the correct consequence of
  the static-slot reading (no per-`run` `code_named` evaluation exists to fill it), and the
  **fail-until-resume** vs **fail-forever** naming gives the two refusal futures honest
  names. One wording nit below.

## 2. R10-A `patch:<n>` minting vs the staging taxonomy

The mint landed in the ruled shape and is mechanically closed:

- **Minting:** `learn_*` mints the installed clause's `+d.name` itself as `patch:<n>`,
  `<n>` = the `d.patch` ordinal the call appends (:1129-1132, :1589-1591); the user `name`
  rides the patch record as an *alias* (:1132-1133, :1591-1592, glossary :136). "Only the
  verb can mint it" is right — the ordinal belongs to the patch being appended.
- **The alias vs `unlearn` name-targeting — clause arm coexists correctly.** `unlearn`
  resolves the clause arg's `+d.name` label cell (:1643-1646); a learned clause's
  `+d.name` is `patch:<n>`; the learned-clause descriptor arm records the same verbatim
  `patch:<n>` (:1614-1618). So `unlearn(clause)` → `patch:3` → latest patch recording
  `patch:3` — name-equality holds end-to-end. The alias never enters the equality: it is
  patch-record metadata, not a lookup key — consistent, though it buys the user nothing at
  the resolver (see nit on the discovery idiom).
- **Dedupe-by-name — consequence stated and correct.** ":1138-1140 `learn_plan` matching
  its alias verbatim against clause `+d.name`s, so a learned clause's minted `patch:<n>`
  is never re-hit and successive learns append rather than self-replace" — verified: the
  alias can never equal a `patch:<n>` (`patch:` refused as a user spelling, :1133-1135), so
  the dedupe is a *deliberate no-op* for learned clauses while remaining live for
  declared/grafted clauses (an alias spelling `demo:Forager.travel` verbatim still hits and
  replaces). Session and replay agree: two same-alias learns → two `patch:<n>` clauses in
  both arms.
- **`learn_plan("Feed","near_water",…)` (:2380-2401) — COHERENT.** The call passes the
  alias, the inline comments name the split ("alias on the patch; learn_* mints the
  clause's +d.name itself as patch:<ordinal>"), and the prose (:2399-2402) states the rule
  and the reason. The `learned_rule("food_water")` guard correctly prevents the re-learn
  the new append-not-replace semantics would otherwise allow.
- **Emission-refused `patch:` vs `learn_method` qualified-success — consistent.** The
  `patch:` refusal covers every user `name` argument at `graft_*`/`learn_*` (:1133-1135) —
  including `learn_method`'s member name, which is right (a `patch:` member could never be
  spelled in the qualified grammar). The mint itself is clause-scoped by the paragraph's
  own opening ("Grafted clauses take `name` parameters", :1129), so `learn_method`'s `name`
  stays the functional symbol lookup name — see scoping nit below on "(and its twins)".
- **`unlearn` LIFO same-name limit (:1142-1145) — landed but premise is stale** — finding 1.

Companion verifications against the code (R10-B/C/E and the minor batch): `historyFromSeal`
`publication.cpp:1093-1103` mints `Store()` at :1097 and calls file-local `applyOpsSegment`
at :1100 — decl `publication.hpp:605-612`; the hydration pair `setContentSource` +
`bindPublishedLocalScroll` is `restorePublication`'s own at :1127-1131, and `addScroll`
dedupes on `sameContentAs` (`store.cpp:1388-1405`) so the adapter can re-add `from` to
recover `self` — the ceremony is mechanizable with existing public API (`bindPublishedLocalScroll`
`store.cpp:1418-1422`, `setContentSource` `:1425`). The `Store()` mint's fresh
`UserPermascroll` is `store.cpp:123-124`; `documentId_` ctor state `store.hpp:133`,`:1435`.
`WorkerContext(base, reader)` two-arg ctor `worker_context.hpp:52-54` calls
`arena.setDefaultSpanReader` in its init list; `resume` passes the manifold alone and calls
it post-construction, `mesh_actor.cpp:270-273` — the stated call shape is exact.
`checkpoint.store` is `unique_ptr<Store>` (`mesh_value.hpp:102`), consumed for
`userPermascrollPtr()` at `mesh_actor.cpp:261`, `sourceLease` pinned at `:268`. The
recorded-enumeration precedent is real (`program.instructions` → `setOpcodeKind`,
`mesh_evaluator.cpp:658-663`), the value-scan hazard is real (`dimConstant` mints small
`int64`s, `compiler.cpp:769-772`; `getOpcodeKind`'s `asInt64 ≤ Linked` fallback,
`vortex_vm.cpp:102-114`), and `opRecords()`/`getCompactOp(index)` (`store.hpp:171-180`,
`:1363-1364`) mechanize the order-vs-order comparand.

## 3. New findings (v1.10 deltas only)

### 1. MINOR — §6.1 :1140-1145 — the `unlearn`/same-name limit was written against pre-mint names and now asserts behaviour the mint removed

The F4 sentence was applied verbatim, but R10-A's mint broke its premise. Under
verb-minted labels every learned clause's `+d.name` is a *unique* `patch:<n>`, so:

- "among same-named learned clauses it always pops the *latest* patch recording that name —
  the clause-cell argument cannot pick an earlier twin out of order" (:1143-1145) — false
  under the mechanism: two same-*alias* learned handlers are `patch:3` and `patch:7`, the
  clause arg resolves its own unique `+d.name` (:1643-1646), so `unlearn` picks each twin
  **precisely**. The described limit cannot occur.
- "`learn_handler` is deliberately additive — a second same-named handler clause *appends*
  beside the first, so `plan_named(name)`'s first-hit stays ambiguous between them"
  (:1140-1142) — stale twice over: same-named at the `+d.name` level is unreachable for
  learned clauses, and `plan_named("foo")` on two same-alias learned handlers yields **zero**
  hits (the alias is not a `+d.name`), not an ambiguous first.

The genuinely surviving limits are different in subject: first-hit ambiguity among
*grafted* verbatim same-named clauses (`graft_*` holds `name` verbatim, :1129) and
LIFO-by-name over *repeated patches to one target* (`learn_where`/`learn_body` re-patches,
`learn_method` re-definitions — which is correct undo order, not a limitation). As written
the sentence misstates two verbs' observable behaviour; an implementer trusting the note
over the mechanism could build alias-keyed `unlearn` and *create* the bug it describes.
(The ergo lane's R11-F1 reaches the same defect independently; I corroborate from the
staging side — the descriptor's learned-clause arm (:1614-1618) and the LIFO ordering are
individually right, only this consequence-note is stale.)

**Fix:** re-key to the mint — "`learn_handler` is deliberately additive: a second
same-alias clause appends beside the first under its own `patch:<n>` label, so
`plan_named` by the user name hits neither (resolve by `patch:<n>` or `clauses_of`), and
`unlearn` targets each precisely by its minted label. Same-`+d.name` ambiguity survives
only among *grafted* clauses; among patches recording one target name `unlearn` pops the
latest — simply undo order."

### 2. MINOR — §6.6 :1643-1646 vs :1614-1616 — `unlearn`'s symbol-arm name equality needs an unstated module-prefix composition

The clause arm of `unlearn`'s reverse read is now clean (`+d.name` = recorded
`patch:<n>`/qualified, verbatim both sides). The symbol arm is not: the patch records the
target's *qualified* `module:Agent.member` spelling for member targets (:1614-1616 —
`demo:Forager.step`), while the arg's reverse read is "the symbol cell's own content"
(:1645) — and symbol cells hold the `Agent.member` spelling *without* the module prefix
(`exportSymbol` mints `makeCell(name)`, `vortex_core.cpp:119-127`; "members as
`Agent.member`" :1069, "the name lives as the symbol cell's *content*" :2072-2074 — i.e.
`Forager.step`). Literal equality then fails for every `learn_method` patch
(`Forager.step` ≠ `demo:Forager.step`) unless the reverse read composes the `module:`
prefix — obtainable (`-d.vars` back to the module cell → its content is the module path)
but never stated. Pre-existing phrasing made load-bearing by R10-A's recorded-name
convention, and directly in the path of "does the alias coexist with `unlearn`'s
name-targeting". **Fix:** one clause — "the symbol arm composes the qualified spelling:
the owning module cell's content as the `module:` prefix plus the symbol's own
`Agent.member` content" (or record the content-form and split at replay — the qualified
form is what `symbol_named`'s pre-split `owner` wants, :2040-2041, so composition is the
right arm).

### 3. MINOR — §6.1 :1130-1132 / §6.6 :1589-1591 — the `patch:<n>` ordinal's mint source is unspecified, and the position reading collides with a live patch

"`<n>` = the `d.patch` ordinal the call creates" never says where the ordinal comes from.
The patch records `{ordinal, …}` as a field and replay walks "in ordinal order"
(:1947, :1995), so the recorded value — not chain position — is the sequence key; but
"the `d.patch` ordinal" most naturally implements as *rank length at append*. Under that
reading a normal `unlearn`→`learn` sequence collides: patches `{1,2,4}` after an interior
drop (`unlearn` pops are LIFO-by-name, so drops leave holes, :1584-1585) → next learn
mints ordinal `4` → a **second live `patch:4`** — a duplicate clause `+d.name`, exactly
the ambiguity §6.7 refuses for head labels ("labels are the re-resolution key, so a
duplicate would make `code_named` ambiguous", :2012-2014) and `reify:` serials ("a
*durable per-agent counter* on `d.vars`… so reified labels never collide", :2048-2050).
`patch:<n>` is now the same kind of re-resolution key — `unlearn` matching,
`plan_named("patch:<n>")`, replay target resolution — and gets no uniqueness rule.
Tail-field+1 minting is safe for interior gaps (tail-drop reuse collides only with dead
patches, since references flow later→earlier and LIFO pops referrers first) but that is an
implementation choice the doc does not pin. **Fix:** one clause — "`<n>` reads the rank
tail's recorded ordinal + 1 (or a durable per-agent counter like `reify:`'s serial),
never the rank length — dropped patches leave holes and a length-derived mint collides
with a live `patch:<n>`."

### Nits

- :1695 — "The emitter knows which future the miss has" overclaims for *verbatim-label*
  misses: a later same-incarnation `learn` may or may not register `L`, which the emitter
  cannot foresee; it can fully classify only qualified misses (head → until-resume,
  head-less `def` → forever). The parenthetical taxonomy is right; soften to "warns that
  the operand is unresolved at emission and names both futures".
- :1587-1589 — "(and its twins)" invites reading the `patch:<n>` mint onto `learn_method`,
  which installs no clause; its `name` is the symbol's functional lookup name (callable
  as `Owner.name`, never an alias). One scoping clause: the mint applies to the
  clause-installing verbs (`learn_plan`, `learn_handler`).
- :1075 — `label_of` is still spec'd for "a head's `+d.name`", but the learned-clause
  rediscovery path (`clause` → its minted `patch:<n>`) is the same `+d.name` read on a
  non-head cell; widen to "a `+d.name`-bearing cell's" or state clauses qualify — the
  doc's own convention is to name each class's re-resolution path (:2058).
- :1403-1405 — "(whose cells all classify state-side as `>E_ops` in any case)" rests on
  V12's state→program-skeleton link bar: a `≤E_ops` cell reachable from the agent-cell
  walk would classify program-side by the ref test regardless of which walk reached it.
  True under the invariant; cite it.
- :1133 — "refused as a user-supplied `name` at `graft_*`/`learn_*` emission" borrows the
  fragment-emission sense for a runtime verb boundary (`graft_*` never emits); "as a
  `name` argument at the `graft_*`/`learn_*` call" is the accurate shape.
- :136/:1592 — the patch cell and its installed clause carry the same `patch:<n>`
  `+d.name` label; disjointness holds only because `plan_named`'s space is clause-rank
  cells (:1073) — worth half a clause saying the shared label is intentional (the patch's
  label names the clause it installed).
- :1745-1749/P16 row — worth one clause stating the corollary: the only *qualified*
  `run`-role record that ever binds names a `learn_method`-installed (head-valued) symbol
  — a declared `def` symbol's `+d.values`→entry-op hop lands head-less by construction, so
  `code_named("demo:Forager.f")` on a declared `f` is a guaranteed patch-drop, not a
  plausible durable spelling.
- :1489 — "`mesh_actor.cpp:182`, consumed at `:186`,`:261`" — `:186` is the `wallNow`
  parameter line; `checkpoint.store` is checked at `:188` and read at `:261`. Micro-cite
  wobble.

## 4. Staging-discipline summary

v1.10 lands the R10 rulings in shape: the `patch:<n>` mint is enforced-by-construction
(verb-only, ordinal-keyed, disjoint from head labels and module names), the lineage-store
seed ceremony is now the full `restorePublication` shape with the unmechanized arm honestly
named, genesis (b) registers the recorded enumeration rather than scanning, and the
comparand is pinned to post-fold order. The staging taxonomy stays closed: emission-fixed
slots, the two-space `code_named`/`plan_named`/`symbol_named` partition, and the
recorded-name conventions are mutually consistent. The new defects are all narrow
specification gaps around the minted name — a stale consequence-note, an unstated
prefix composition, an unpinned ordinal mint — not new architecture.

VERDICT: CONVERGED — 0 blocking, 0 major, 3 minors (stale `unlearn` same-name premise;
`unlearn` symbol-arm needs module-prefix composition; `patch:<n>` ordinal mint source
unspecified — position reading collides live), 8 nits.
