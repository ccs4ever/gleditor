# Round 10 critique — Programming Language Design & Ergonomics (v1.9)

Scope: v1.9 deltas only — verification of the three r9 minors, plus the R9-A landmark split
(`E_ops` + per-cell classification + mandatory ops-segment content comparand, :1794-1850,
:600-624, :1361-1471), the R9-B spawn-side base-injection seam (:1394-1417, :2390-2403,
:2428-2437), the R9-C capture-endpoint pre-hydration restatement (:1853-1887), and the minor
batch (:1058, :1060, :1114-1122, :1573-1586, :1622-1636, :1961-1964, :2443). Cross-checked
against `vagent-agent-language.md` (`##` anchor semantics, `[Cell]` results, agent cell vs
`home`) and `vagent/cell_path.cpp` + `compiler.cpp`.

## r9 verdicts

- **R9-F1 (qualified-`L` `code_named` `run` route, MINOR)** — **RESOLVED as ruled.** The
  unresolved-at-emission convention is stated end to end: the emitter records `L` verbatim,
  resolves it through `code_named`'s two-space lookup at emission, writes `int64(head)` on a
  hit and leaves the slot head-less on a miss — "a qualified spelling like `demo:Forager.f`
  that no head label can carry, or a `reify:`/`d.code` label the emission set cannot yet
  contain" (:1622-1630) — and the static-slot reading is explicit ("there is no per-`run`
  `code_named` evaluation to fail or mask it", :1630). The genesis side routes the qualified
  `run`-role record through the qualified path with the head-required hop (:1680-1686), the
  head-less `def` outcome is the named **headless-`run`** refusal (:1684, :1154-1157), and
  P16's row carries the same wording (:2443). The stale "fails `code_named`'s `d.code` walk in
  session" phrase is gone. The route is no longer dead at both ends — it fails every session
  and can succeed post-resume — which leaves a residual surface wart, R10-F3.
- **R9-F2 (declared-handler gap, MINOR)** — **RESOLVED fully.** The gap now names all three
  verbs' worth: unpatchable ("`learn_where`/`learn_body` on a *declared* handler hits the same
  refusal"), un-*replaceable* and un-*removable* durably ("dedupe-by-name cannot match a
  nameless clause, so `learn_handler` on the same event is always additive — it coexists with
  the declared clause under `priority` ordering rather than superseding it — `unlearn` has no
  recorded name to match, and `prune` is session-only; post-resume the image folds the
  declared clause back while the learned shadow replays alongside it", :1573-1582) — exactly
  the extension the finding asked for. The additive-not-deduped half is stated at :1120-1122
  with the `plan_named(name)` first-hit ambiguity and `priority` as the differentiator. One
  parallel ambiguity in the same family remains unstated — R10-F4.
- **R9-F3 (`label_of` acquisition path, MINOR)** — **landed textually, with a new defect.**
  The row now names the `cells"…"` route and the `Code<Cell>`→`Cell` reason (:1060), and §6.7
  repeats it (:1961-1964). But the registered-head spelling it gives is anchored on the wrong
  cell — R10-F2.

## The questions posed

**Is the `code_named`-verbatim convention discoverable (does a user know `patch:<n>` matches
but `mod:name` doesn't)?** Partially. §6.7 is honest: first-`:` splitting never applies to
`code_named` ("`demo:Forager.f` is just a label", :1941-1946) and `patch:` is "*a clause-label
prefix … never a head label*" (:1672-1673). So `patch:<n>` matches `plan_named` (verbatim
clause `+d.name`, :1058), never `code_named`; `mod:name` matches `symbol_named`/`plan_named`,
never `code_named`. The gap is at the point of use: the `code_named` row (:1059) says only
"`d.code` rank lookup by `d.name` label" — a reader working from the table can try
`code_named("patch:3")` (it *looks* like the `src:`/`reify:` prefix family) or
`code_named("demo:Forager.f")` and get a bare empty-`Opt` statement failure with no "wrong
space" hint. One inline clause in the row would close it (nit below).

**Is the anonymous-handler gap surfaced understandably?** Yes — the refusal is named at the
`learn_*` boundary (:1572-1573), the durable immortality is spelled out, and the coexistence
mechanism (`priority` ordering, `plan_named` first-hit) is stated where the user will look
(:1120-1122). Understandable and honest; residual nit in R10-F4.

**Do the `E_ops` mechanics leak into anything user-facing?** No — cleanly confined. `E_ops`,
the per-cell `≤ E_ops` classification and the comparand live in §4.5's lineage paragraph, the
§6.6 sideness/invariant machinery, genesis steps (a)–(e), and §10's adapter text — the layers
where they belong. The user contract stays "a learnable fragment is *self-contained*"
(:1597-1607), and the one user-visible consequence is a *named* diagnostic — `learn`'s
lineage refusal reports *no program lineage*, distinct from closedness (:970-971; the r9 nit,
adopted). No signature, diagnostic or example exposes the bound itself.

## New findings (v1.9 deltas)

### R10-F1. MAJOR — the `patch:` reservation orphans its own convention and breaks §9.3

v1.9 newly states "`patch:` is refused as a user-supplied `name` at `graft_*`/`learn_*`
emission" (:1115-1117, changelog :2995) — but the doc's flagship worked example passes exactly
that: `learn_plan("Feed", "patch:near_water", …)` (:2286). Under the rule as written the §9.3
handler's `learn_plan` fails at emission; the example also violates the companion pinning
(`<n>` is "the patch's `d.patch` ordinal", :1115 — `patch:near_water` carries no ordinal).

The deeper defect is upstream: the reservation protects a convention **nothing can mint**.
Every `graft_*`/`learn_*` `name` is a user-supplied argument (:1067-1068, :1073), and no
auto-naming surface is described — so under the refusal, `patch:<n>` clause labels can never
exist, leaving the descriptor's verbatim `patch:<n>` arm (:1558-1561), the head-layout
guard's "largest producer" rationale (:1692) and the module-declaration reservation
(:1947-1950) guarding a convention with no producer, and `plan_named("patch:3")` permanently
unmatchable. An implementer must either invent an unstated name-defaulting rule, ship a dead
convention, or drop the reservation and reopen the ordinal-collision hole it exists to close
(a forged `patch:3` colliding with the real third patch → `unlearn`/`plan_named` misbind —
the collision the `reify:`-serial and §6.7 reservations elsewhere exist to prevent).

Fix (pick one): **(a)** scope the refusal to *ordinal-shaped* spellings — refuse
`patch:<integer>` user names (protecting the `<n>` ↔ `d.patch` ordinal correspondence) while
permitting `patch:<ident>` — and change the example's label is then unnecessary; **(b)** make
`learn_*` auto-mint `patch:<n>` (default `name`), keep the prefix fully reserved for the
system, and change §9.3's argument to a user name (`"near_water"`); **(c)** keep the blanket
refusal and recast `patch:<n>` throughout as adapter-minted-only with the minting surface
named. (b) is the most coherent: the ordinal correspondence becomes *enforced* rather than
conventional, and `plan_named("patch:<n>")` is then trustworthy by construction.

### R10-F2. MINOR — `label_of`'s registered-head path is anchored on the wrong cell

The row names "`cells"##/+d.code"` for a registered head" (:1060). But `##` is
`AnchorKind::Home`, lowered to `core_.home()` (`compiler.cpp:749-750`) — the arena home,
i.e. the persistent program-image root `initGenesis` adopts (`*base->home()`,
`vortex_core.cpp:30-36`; spec :1257, :1263 "reads home's `d.items` rank") — while `d.code`
is **the agent's** registration rank (:1346 "the agent's registration rank", :1538 "on the
agent cell", :1912 "agent → fragment heads"). `home` ≠ `agent_cell()` — they are two distinct
adapter bindings (:986) — so `cells"##/+d.code"` reads home's (nonexistent) `+d.code`
neighbours and yields nothing. The doc's own §9 example spells agent-cell rank access
correctly: `let a := agent_cell()` then `cells"$a/+d.synapse"` (:2142-2143). Same-class slip
in miniature: both example paths yield `[Cell]` (a traversal's result is a list head, spec
:1271) while `label_of` takes `Cell` — the index/`for` unwrap is elided.

Fix: "`cells"$a/+d.code"` over the registered rank (`a := agent_cell()`), `cells"$clause/
+d.spin"` for the installed copy — each a `[Cell]` the caller indexes".

### R10-F3. MINOR — the genesis-resolvable `run` operand is indistinguishable from never-resolvable at the point of failure

The unresolved-at-emission convention is correctly *stated* (:1626-1630), but in-session a
head-less-slot `run` refuses at the §6.2 descriptor check identically whether `L` will
resolve at genesis or never can — the user sees one refusal for two very different futures
(post-resume success vs the learned patch dropping outright). The emitter *knows* which it
is — it recorded `L` on a lookup miss — and this is the one spelling where in-session
behaviour provably diverges from resumed behaviour in the fail→work direction. Fix: an
emission-time note/warning when the two-space lookup misses ("`run` operand `code_named(L)`
unresolved at emission — fails in-session; rebinds at genesis only if `L` then lands on a
head, else the patch drops as unresolved"), plus a clause in `code_named`'s row marking the
space ("head labels only — qualified names are `symbol_named`/`plan_named`'s space, clause
labels `plan_named`'s").

### R10-F4. MINOR — `unlearn` cannot target among same-named learned handler clauses

The additive `learn_handler` rule (:1120-1122) creates a second first-hit ambiguity the doc
doesn't name: `unlearn` matches "*the latest* patch whose recorded target name equals" the
argument's resolved name (:1583-1586), so with two learned `foo` handler clauses,
`unlearn(<earlier foo clause>)` still resolves to `foo` and undoes the *later* `foo` patch —
the clause-cell argument cannot discriminate at all. LIFO-by-name is consistent with
name-keyed `unlearn`, but the consequence (you cannot remove a specific same-named handler
out of order; only the latest `foo` patch pops) deserves the same sentence the `plan_named`
first-hit ambiguity got. Fix: extend :1120-1122 or :1580-1582 by one clause — "and `unlearn`
on any of them pops the *latest* patch recording that name, not the clause passed".

### Nits (not findings)

- `learn_plan` replacement needs the **qualified verbatim**: dedupe-by-name matches `+d.name`
  verbatim (:1118-1120), and a declared plan's `+d.name` holds `demo:Forager.travel`
  (:1556-1558, :1978-1980) — so `learn_plan("Feed", "travel", …)` *coexists* with the
  declared clause instead of replacing it, the same silent-coexistence shape the handler gap
  now names. A "same-named means the stored `+d.name` verbatim — qualified for declared
  clauses" clause in :1118-1120 closes it.
- :2107-2109 lists "a synapse junction's own label" among "anything
  `symbol_named`/`code_named`/`plan_named` can resolve at rehydration" — junction cells sit
  on `+d.synapse`, in no named resolver's space (not heads, not clauses, not `d.vars`
  symbols). Pre-dates v1.9; noted while in the surface.
- The retained qualified-`run`-role path is now honest, but its *only* success case is a
  `learn_method`-installed (head-valued) symbol — every declared `def`/`method` resolves
  head-less and drops the patch at genesis. Worth one clause saying so, so nobody reads
  "the only route to the qualified resolution path" as a general feature.

## Summary

All three r9 minors landed as ruled: the unresolved-at-emission convention with the
static-slot reading (:1622-1636), the full declared-handler gap (:1573-1582, :1120-1122), and
the `label_of` `cells"…"` path (:1060, :1961-1964). `E_ops` stays invisible to user code.
But the v1.9 minor batch introduced a real regression: the `patch:` `name` reservation
(:1115-1117) contradicts §9.3's `learn_plan("Feed", "patch:near_water", …)` (:2286) and leaves
`patch:<n>` with no minting surface, dead-ending the descriptor arm built on it (R10-F1,
MAJOR). Three minors: the `label_of` row's `cells"##/+d.code"` anchors `home` where `d.code`
lives on the agent cell (R10-F2), the genesis-resolvable `run` operand shares one refusal
signal with never-resolvable (R10-F3), and `unlearn`'s LIFO-by-name ambiguity among same-named
handlers is unstated (R10-F4).

VERDICT: NOT CONVERGED
