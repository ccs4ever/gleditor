# Round 8 critique — `design/vagent-metaprogramming.md` v1.7 — Metaprogramming Expert lane

Scope: verdicts on my r7 findings (the (e)-before-(f) name-record sweep major, the
`src:`/`reify:`/`patch:` reservation enforcement minor, the `code_named` record-key minor) against
the v1.7 deltas (:1650–1700 genesis reorder, :1455–1467 patch-target split, :1517–1540 slot-role
writeback, :1718–1739 labels, plus the R7-A lineage text they sit inside), plus staging review of
the deltas only.

## 1. Verdicts on my r7 findings

**r7-major (name-record sweep ran before `d.patch` replay) — CLOSED, including the
copy-inheritance concern.** The genesis order is now (a) lineage bind (:1657–1666), (b)
`ensureDimension` find-or-mint + `setOpcodeKind` pass over the folded image (:1667–1675), (c)
`programRef` bind (:1676–1677), (d) `d.patch` replay in ordinal order (:1678–1681), **(e) `+d.refs`
rehydration, last** (:1682–1699), (f) turns (:1700). This is not the naive swap I warned against:
the sweep covers "every `learn`-registered head's records **and every replay-installed copy's
regenerated records**" (:1682–1683), and record regeneration is mechanized — `reify` regenerates
the copy's `+d.refs` "records remapped to the new ref cells, name-records re-linked" (:1038,
:1074–1075). Copy-inheritance is named as the deciding dependency, twice (:1679–1681: "the copy
inherits the registered head's slot contents at mint — the copy-inheritance that decides (e)'s
position"; :1691–1695), and the post-installation-symbols arm quotes back exactly the misbind/drop
class my finding exhibited (:1696–1699: "a call to a patch-replaced symbol bound the
pre-replacement image binding and a call naming a patch-installed symbol dropped the patch"). The
divergence clause is reconciled — the learned re-point "replayed at genesis step (d) is picked up
exactly because the `+d.refs` sweep runs after replay" (:1570–1571) — and the three other statement
sites agree (:378–381, :589, :2218). One mechanism nit inside the justification survives as new
finding 1; it does not reopen the verdict.

**r7 minor 2 (`src:`/`reify:` reservation unenforced, `patch:` unreserved) — CLOSED.**
:1749–1751 names the enforcement point and adds `patch:`: "The emitter's automatic prefixes are
**reserved at module declaration**: `src:`, `reify:` and `patch:` may name no module — a module
declaration carrying one is a compile-time refusal." The contradictory "a module literally takes
that name" arm is deleted; in its place §6.6 argues disjointness through the reservation plus the
`#label`-can't-spell-`:` grammar fact (:1537–1542). The qualified/verbatim channels are now
provably disjoint.

**r7 minor 3 (`code_named` record key ambiguous) — CLOSED.** :1504–1508 is the requested clause
verbatim: "`code_named(L)` operand with `L` a string literal → `L`; … any operand reached through
a non-syntactic path — a `let` alias, a computed argument — is opaque, so an opaque or
`reflect`-born `run` target in a learnable fragment is a closedness refusal at emission."

**r7 micro-nits — applied, one carried.**

- Co-emission proxy invariant — *mooted by a better mechanism*: the slot convention is now carried
  on the record itself as `+d.grab → "call"`/`"run"` role label "never derived from name shape"
  (:1529–1533; legend :985–988; P16 :2218). A future record kind can no longer reintroduce R6-B
  inverted, which is exactly what the nit asked to guarantee.
- Label→head walk ambiguity — resolved: restricted to "cells in the head layout (`+d.entry` plus
  the descriptor `+d.values` — the shape restriction that keeps clause labels and qualified symbol
  names on the same dim from matching)" (:1557–1560).
- `home` edge invariant — resolved verbatim: "nothing mints a `+d` edge *into* `home`" (:1390–1394),
  with `home` correctly the persistent base home `initGenesis` adopts (:1384–1389).
- `+n`/`+o` composition — resolved (:1736–1738: ordinal appends to whatever chain the site
  produced; a clone of a nested literal keeps its `+o:c` arms).
- **Carried:** `+n` clone-ordinal counter scope and allocation order still unstated — the label is
  a durable re-resolution key, so cross-recompile determinism must be asserted or the
  EDL-refresh expectation named (nit 2 below).

**R7-A lineage text (context for the deltas) — sound where it touches my scope.** The
arrives-folded narration ("the persistent manifold **arrives — it is not constructed**",
:1650–1655), `E` = image prefix length on the `programRef` bind record (:1350–1353, :1657–1659),
the byte-identical-verbatim requirement (:1342–1344), `setOpcodeKind`-pass-not-`mintOpcode`
(:1669–1675, rationale correctly on the empty per-`WorkerContext` map), and the fail-closed arm
(:1367–1375, cross-referenced :1663, :1764, :2450) are all consistent with the reorder.

## 2. New findings (v1.7 deltas only)

### 1. MINOR — :1692–1695 — "an `OpHandle`-kind source slot yields an `OpHandle`-kind copy" is impossible under `reify`'s own mechanism

`reify` replays the recipe "through `New`/`Link`/`Value`" (:1070) — opcode-level writes that cannot
mint handle-kind cells. `OpHandle` arises only through `setValueBits` in the ceremony or
`importValue`'s remap — which *demotes* stray `OpHandle` cells to `None` (:1687–1690), the opposite
of yielding them. What the copy can inherit is the slot's **content**: an `OpHandle` source slot
contributes its handle target as a plain `int64` — i.e., a replayed copy's ref slots arrive
*pre-rehydrated* as `int64(promotedTarget)` (or, under recipe-literal snapshots, the emission-time
persistent ref — durable either way), and the sweep's ref arm is a kind-guarded no-op on them
(:1643–1645). The load-bearing arm for copies is the **name-record** rebind (stale int64 →
post-installation rewrite), which the text also states — so the sweep's scope and conclusion stand.
But the stated mechanics are wrong and will mislead an implementer into expecting handle-kind cells
on replay-minted copies. Say instead: "the copy inherits slot *contents* — an `OpHandle` slot's
target lands as Int64, so ref slots self-rehydrate; operand slots carry the head's last write, so
name-record copies still need the rebind arm."

### 2. MINOR — :1505–1506 — a `plan_named`-sourced `run` record can never re-resolve, and the arm's typing bridge is unstated

The new emission arm records "a `plan_named`/`symbol_named` operand spelled with a literal
qualified name → that qualified name". Two problems: (a) genesis's qualified path resolves every
`module:`-shaped name through `module_named` + `symbol_named` on the module's `d.vars`
(:1543–1544), but plans live on clause `+d.name` labels, never `d.vars` (:1775–1776) — a
`plan_named`-sourced name misses `d.vars` guaranteed, so the record always faults the patch at
genesis. Fail-closed, but an emission-time trap: a fragment that passes `closed` carrying such a
record is silently un-learned every resume. (b) Both `plan_named` and `symbol_named` return
`Opt<Cell>` (:1026, :1030) while `run` takes `Code<T>` (:1037) — the path by which a resolved
`Cell` reaches a head-valued `Code` operand is never given (and `body_of`/`test_of` would make it
reflect-born → refused). Fix: restrict the arm to `symbol_named`-born operands whose resolved
symbol binds a head (the headless-`run` arm already prices the `def` case), and state the
`Cell`→`Code` typing bridge — or drop `plan_named` from the arm entirely.

### 3. MINOR — :1733–1735 — nested-literal label: "innermost `src:` site" reads the wrong end; arm direction unstated

"A *nested* literal … labels from its **innermost** `src:` site and chains `+o:c` per nesting
level: `src:m:l:c+o:c'…` where each `+o:` arm is the inner-offset position §5.1 uses." For the key
to be unique, the base `src:` must be the **outermost** enclosing site (or expansion site): the
nested literal's own `loc` is retained verbatim by `cloneExpr` (:942–943), so two expansions of
one template's nested literal share it, and "inner-offset" arms can only name positions *inside*
the base's body — they descend, they cannot point outward. As written, "innermost `src:` site" +
inward `+o` arms inverts the construction and leaves each arm's referent (offset within which
enclosing body, in which order) unstated. Misread, two differently-sited expansions mint colliding
durable labels — the exact class this section exists to exclude. One clause fixes it: "`src:` is
the outermost enclosing literal's site; `+o:c` arms descend innermost-first…" (or whichever order
is intended — say it).

### 4. Micro-nits

- :1736–1744 — a user `#label` inside a twice-expanded body: `+n` ordinals attach to `src:` labels
  only, so a `code"#foo …"` literal inside a twice-expanded template mints two heads labelled
  `#foo` — either the merged-emission-set uniqueness check refuses (labeled literals are then
  un-cloneable — say so) or clones take `#foo+n`. Unstated.
- :1737 — `+n` counter scope/allocation order still unstated (carried from r7): the label is a
  durable re-resolution key; a recompile numbering clones differently mints labels checkpointed
  records cannot match → dropped patches on artifact rebuild.
- :2218 — the P16 row's run-role gloss ("resolves its `src:`/`reify:`/`#label` through the
  program-image label→head lookup") omits the qualified-run sub-case §6.6 now admits
  (`symbol_named` operand → qualified path → `int64(head)`, headless-`run` on a `def` symbol,
  :1549–1555); the row names the refusal but not its trigger.
- :1764, :2450 — "classifies prefix-less" conflates the lineage-level property (the store carries
  no image prefix) with a cell-level one (a foreign head outside the *bound* lineage); a cell
  inside a foreign lineage is not prefix-less. Say "outside the bound `programRef` lineage".
- :1454–1455 — "the three-part name intentions use" over-covers: goal/event targets resolve as
  `module:Name` (bare `d.vars` names, :1771–1773) and learned-clause targets record `patch:<n>`;
  scope the parenthetical to program-original clause/member targets.
- :1667–1675 — the `setOpcodeKind` pass enumerates "the folded image's op cells" by an unstated
  walk (position band `1..E` vs structural-dim walk); name it.

## 3. Staging discipline summary

The reorder lands with the dependency analysis the ruling demanded: copy-inheritance is named as
*the* reason the sweep must be last, scope is dual (registered heads + replay-regenerated copy
records), and `reify`'s record regeneration was already mechanized (:1038, :1074–1075) — the
ruling's "second sweep" is correctly realized as one post-replay sweep over both record sets. The
role-label change is a genuine improvement over my suggested invariant — records self-describe
their slot convention, retiring the shape-as-proxy hazard class. The residuals are all in the
*reasoning layer*, not the ordering: an impossible `OpHandle`-copy claim doing work the
kind-guard already makes safe (finding 1), a `plan_named` arm whose recorded names have no
re-resolution path (finding 2), and a label-construction clause whose direction inverts on a
careless reading (finding 3). Nothing in the deltas reintroduces a stale-or-dropped binding.

VERDICT: CONVERGED at the major level — r7-major closed (replay-before-sweep, copy-inheritance
named and scoped), both r7 minors closed, nits applied with `+n` determinism carried. New: 0
blocking, 0 majors, 3 minors (OpHandle-copy mechanism claim; `plan_named` run-record re-resolution
hole; nested-literal label direction), 6 nits — polish for v1.8, none gating.
