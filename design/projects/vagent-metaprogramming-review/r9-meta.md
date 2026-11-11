# Round 9 critique — `design/vagent-metaprogramming.md` v1.8 — Metaprogramming Expert lane

Scope: verify my three r8 minors landed; re-examine the staging story after the
`plan_named`/`symbol_named` `run`-operand drop; the adapter-owned publish `Store` (R8-A) as it bears
on staging; `patch:<n>` as a clause-target descriptor arm vs the quasiquote/label conventions; hunt
new defects in the deltas only.

## 1. r8 minors — all three landed

- **OpHandle-kind copy (:1780–1791) — CLOSED.** The (e) sweep now states the correct mechanism:
  "`reify`'s `New`/`Link`/`Value` replay cannot mint handle-kind cells, so a source `OpHandle` slot
  contributes its handle *target* as plain Int64 and the copy's ref slots arrive pre-rehydrated (the
  ref arm's kind-guard makes it a no-op on them) — while operand slots carry the head's last write,
  so the copies' *name-record* slots still need the rebind arm" — the dual-scope sweep (registered
  heads + (d)-handed install set, :1770–1774) is consistent. One wording regression inside the
  adopted text — new finding 3.
- **`plan_named`-sourced `run` record arm — CLOSED.** Dropped outright as ruled: "there is **no
  `plan_named`/`symbol_named` arm** — both builtins return `Opt<Cell>` where `run` demands
  `Code<T>` and M3 bars the silent `Cell`→`Code` upgrade, so such a `run` never typechecks and
  nothing is recorded — the head-less `def` symbol it would name is the headless-`run` refusal
  class" (:1569–1571); the session-side surface is named at :1146–1149; the `Cell`→`Code` bar is
  M3's refused-alternative (:2360). `Cell`→`Code` remains barred.
- **Nested-literal label direction — CLOSED.** ":1828–1834 now labels a nested literal "from the
  **outermost** enclosing literal's `src:` site and chains `+o:c` descending inward per level", with
  arm order spelled: "the first arm sits inside the `src:` literal's body, the last names the
  labelled literal's own spot in its immediate parent". The two carried nits are also resolved:
  `+n` counts "that site's clones in expansion order, deterministically" (:1838–1840), and a user
  `#label` inside a multiply-expanded body is "refused — labelled literals are not cloneable"
  (:1845–1847). Other carried nits applied: (b)'s op-cell walk is named as "(a)'s" (:1754); the P16
  row carries the qualified-run trigger (:2336); "outside the bound `programRef` lineage" phrasing
  landed (:1872).

## 2. The posed questions

**Does dropping qualified-name `run` leave a coherent staging story?** Mostly yes. The dropped arm
was dead text three lanes agreed on — `plan_named`/`symbol_named` return `Opt<Cell>`, `run` wants
`Code<T>`, M3 bars the upgrade, and the clause-name/`d.vars` split meant the recorded names had no
re-resolution path. Its removal loses nothing a session could ever have executed. The retained
route — `run(code_named "m:N")` — is the right *shape* for the escape hatch: a grafted/learned
method symbol's `+d.values` names a *head* (:1202–1203), so the qualified path genuinely has heads
to land on, and genesis's role-split rebind (qualified path + head-or-op hop + `run`-role
`int64(head)`, :1611–1621) is the disciplined version of "run a learned method by name". The loss
is semantically consistent because `plan_named`'s referent is a clause cell, never a fragment head —
running "a plan" means running its test/body heads, which label-resolution already covers. **But**
the session-side story for the qualified arm is misdescribed — finding 2.

**Does the adapter-owned store affect staging semantics?** No defect found. The seam is honestly
priced as owed adapter-API surface alongside P4's wake (:1648–1653, :2295–2301, :2326–2330); the
`spawn()`/`resume()` cite is accurate per the r8 lineage verification. Position-stable (not
byte-identical) seeding is the correct requirement — `applyOpsSegment` remaps `ScrollId`s, so what
must be identical is the op sequence at `1..E`, which is exactly what `int64(position)` emission
bindings and name-record re-resolution depend on; scroll-level remapping touches neither. The
"supplies or verifies" fork fails closed through (a) either way. One residual classification nuance
it *creates* — via (a)'s assertion quantification — is finding 1.

**Does `patch:<n>` interact correctly with quasiquote/label conventions?** Yes, with one unstated
rule (finding 4). The disjointness machinery holds: `patch:` is reserved at module declaration
(:1854–1856) so a `patch:<n>` string can never route down the qualified path; user `#label`s cannot
spell `:` (:1848–1849); the head-label lookup is restricted to head-layout cells precisely so clause
`+d.name` labels cannot match (:1624–1628); `plan_named` and patch-target names take the first-`:`
split only when the segment resolves as a module, else the whole string is the name (:1850–1853) —
so `plan_named("patch:3")` verbatim-matches the clause label. The descriptor stores `patch:<n>`
verbatim, never canonicalized `module:Agent.patch:<n>` (:1506–1508), and the anonymous-clause gap
is extended correctly to declared handlers (V15 mints them no `+d.name`, :1520–1524).

## 3. New findings (v1.8 deltas only)

### 1. MAJOR — §6.6 step (a) :1732–1736 — the positional assertion quantifies over an *un-pruned* `program()` walk; in the configuration the doc itself admits, every stateful resume fails genesis

The new positional form says "every cell the `program()` structural walk reaches must hold a
persistent ref `≤ E`, and the walk's maximum must equal `E`". The `program()` walk is the sideness
walk (:1349–1352): outbound-following over the full structural vocabulary — `d.vars`, `d.belief`,
`+d.code`, `+d.values`, `+d.step`, … all included — with `≤E` as a *separate* per-cell conjunct, not
a bound on the walk. The walk's reachable set therefore contains post-E persistent state whenever a
program-side anchor carries it — and the document explicitly admits the primary anchor: the
state-side test's root exemption is written for "a program-side `agent_cell()`" (:1424–1425). If the
agent cell classifies program-side (≤E), the walk follows its outbound rank-membership edges —
`agent +d.vars → member records` (the durable `reify:` serial counter lives there, :1858–1859;
member-var cells too, :1543–1544), `+d.belief → predicate cells`, `+d.code → registered heads` —
all post-E persistent cells, all violating "must hold a persistent ref `≤ E`". Consequence: genesis
refuses the first resume of any agent that has reified once or carries member state — `learn`'s
whole durability story bricks in exactly the configuration the exemption contemplates.

If the intended invariant is instead "no program-side anchor ever carries post-E state" — i.e.,
`agent_cell()` is always state-side — the exemption's case is vacuous and the doc should say that
(and the walk vocabulary's `d.vars`/`d.belief` inclusions become the reason it holds). Otherwise the
walk needs an extent bound. Fix, one clause: "the walk descends only through cells holding
persistent refs `≤ E` — a reached `>E` cell is the boundary, not a violation — and the reached set's
positions must cover `1..E`". Two sub-points:

- **Coverage, not maximum.** "max `= E` + all reached `≤ E`" bounds the band but cannot detect the
  named threat "any non-image cell inside `1..E`" — an unreached foreign occupant inside the band
  evades both clauses (and `home` — "the store's first MakeCell, a pre-extent persistent ref",
  :1434–1435 — may be a legitimate non-image occupant a coverage-equality check must exempt). The
  honest detector is the stated "stronger optional comparand" (the adapter's seeded ops segment,
  :1739–1741); the walk test should either claim boundedness+completeness-of-reach or drop the
  non-image-inside clause.
- Conversely, the `>E`-boundary reading keeps the twin-detection: a merged/re-seeded lineage
  (`mergeValues` → fresh store, :1737–1738) lands image cells *above* `E` too, so pruned-or-refused
  the reached set misses the band and fails closed — the intended arm is preserved.

### 2. MINOR — :1565–1571 — the qualified-`L` `run`-record's session mechanism is misdescribed; under the reading its own sentence implies, the arm is as dead as the one it replaced

"a `code_named(L)` operand with `L` a string literal → `L` verbatim — including a qualified-looking
`L`, the only route a `run`-role record has to the qualified resolution path (it fails `code_named`'s
`d.code` walk in session, but the record it leaves resolves — or drops — at genesis)". Two readings,
neither stated:

- If `code_named` in `run` operand position is a **runtime-evaluated call** — what "fails
  `code_named`'s `d.code` walk in session" literally describes — the operand slot is the call's
  output cell, and genesis's `writeValue(slot, int64(head))` is dead: post-resume `code_named`
  still never takes the qualified path (first-`:` splitting is scoped to `plan_named`/patch-target
  names and `symbol_named`'s pre-split `owner` — "never to `code_named`", :1850–1853), so the next
  `run` evaluates it again, the empty `Opt` fails the statement at the operand read, and the record
  resolves-then-is-never-consumed — the same "no path to effect" defect the dropped `plan_named`
  arm carried.
- If the operand is a **static slot** — which §6.2's "the emitter writes `int64(head)` into it"
  (:1123) and the divergence clause's "a session `run` retains the *emission-time* binding" (:1635)
  both imply — then `code_named` is emission-resolved sugar, no session walk exists to fail, and the
  real semantics is: an `L` unresolvable at emission (qualified names, `reify:`/`d.code` labels the
  emission set cannot yet contain) leaves the slot *unresolved* and `run` refuses it at the
  descriptor/headless check until genesis's record fills it. That reading is coherent — it is the
  disciplined escape hatch the section wants — but the unresolved-at-emission slot convention (what
  content, and that `run` fails it *cleanly* rather than derefs it) is unstated, and the
  session-failure sentence names the wrong mechanism.

Fix: say the operand lowering — e.g., "`code_named(L)` in `run` operand position is a name, not a
call: the emitter resolves `L` through the two-space lookup at emission; an `L` it cannot resolve
leaves the slot head-less, which `run` refuses at the descriptor check until the record's genesis
rebind fills it — so a qualified `L` fails in-session without any `code_named` evaluation" — or, if
the dynamic-call reading is intended, drop the qualified-`L` sub-arm as dead and keep `run`-role
records label-only. (The same unstated convention covers `reify:`/`d.code`-label `L`s, which are
likewise unresolvable at emission.)

### 3. MINOR — :1785–1790 — "the registered head's *current* contents" / "the head's last write" contradicts §6.1's recorded-emission-template rule

The adopted copy-inheritance fix says a replayed install "mints its slots with the registered head's
*current* contents" and that "operand slots carry the head's last write". But `reify` replays "the
*recorded emission template* — the recipe the emitter wrote — because the fragment graph's shape is
fixed at emission; copying the *current* graph (after in-arena mutation) is the U-M7 walk"
(:1096–1098). The copy inherits the recipe's *emission* contents, not current/last-write contents.
The conclusion survives — a ref slot's recorded literal is `int64(persistentPosition)`, stable
across the lineage and equal to what the promoted handle target names (promote's gate excludes
base-bound targets and restore writes Int64 back unchanged, :1721–1723) — but the mechanism phrase
is wrong twice over, and the divergence is live exactly where the sweep cares: a registered head's
name-record slots are rewritten by (e) itself on every genesis, so "current" vs "emission" differ
on the second resume; a retargeted ref slot would likewise copy its emission literal. Say "the
recipe's recorded contents" (and note they are stable *because* they are persistent positions), not
"current"/"last write". (The slippage originates in my r8 suggested wording — flagging it honestly.)

### 4. MINOR — :1106–1108, :1506–1508 — `patch:<n>`'s `<n>` source is unstated, and the convention is collision-open to user-supplied clause names

Learned clauses "are conventionally named `patch:<n>`" — but what `<n>` denotes (the `d.patch`
ordinal — durable-unique by monotonicity — vs a per-agent counter vs a free choice) is never said,
and `graft_plan`/`learn_plan` take `name` as a plain `Str` (:1059), so a user can spell `"patch:3"`
directly. A collision is a real misbind, not a style issue: `plan_named` is first-match-wins
(:1109–1110), the descriptor resolves `patch:<n>` verbatim (:1507), and `unlearn` matches "the
latest patch whose recorded target name equals" the reverse-read name (:1525–1528) — two clauses
named `patch:3` make all three resolve the wrong patch. Fix: reserve `patch:` for `learn_*`-minted
clause names the same way `src:`/`reify:`/`patch:` are reserved at module declaration (a user
`name` carrying it is refused at `graft_*`/`learn_*` emission), and pin `<n>` = the patch's
`d.patch` ordinal (or a durable per-agent counter — the `reify:` serial's source is the model).

### 5. Micro-nits

- :1830–1831 — `+o:c`'s `c` unit is still unstated: `src:m:l:c`'s `c` is a column; the `+o` arm's
  `c` is "the offset position §5.1 uses inside its enclosing body" — a char offset into the body
  text? a line:col pair? Opaque labels make it mechanically safe, but the label grammar is the
  re-resolution key — say the unit.
- :1626–1628 — the head-layout restriction's stated purpose now covers *clause* `+d.name` labels;
  worth one word noting `patch:<n>` is the largest producer of those — currently implied only.
- :1567 — "fails `code_named`'s `d.code` walk" predates the fallthrough; if the sentence survives the
  finding-2 fix at all, `code_named`'s lookup is `d.code` *then* the image-label walk (:1863–1867).

## 4. Staging-discipline summary

The r8 batch landed cleanly and the staging architecture is in better shape than v1.7: the
record-role split (name shape selects the resolution path, `+d.grab` label selects the slot
convention) is now airtight, the label grammar is fully deterministic, and the adapter-owned store
makes the lineage invariant reference a real object with an honest owed-API price. Two live defects:
the (a) assertion was rewritten positionally without bounding the walk — the admitted
program-side-`agent_cell()` configuration turns it into a genesis refusal on durable state
(finding 1); and the qualified-`L` `run` escape hatch's session story names a mechanism that, taken
literally, dead-ends the record's write (finding 2). Findings 3–4 are wording/edge polish in the
same cluster.

VERDICT: NOT CONVERGED — 0 blocking, 1 major (genesis (a)'s un-pruned walk quantification), 3 minors
(qualified-`L` `run`-record session mechanism; "current contents" vs emission template; `patch:<n>`
`<n>` source/user collision), 3 nits.
