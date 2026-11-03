# Round 6 critique (vortex role): vagent-metaprogramming.md v1.5

Lane: VortexCore/VM execution semantics, trail/marks, handle relocation, promote,
MeshActorRuntime invariants. Convergence check on the v1.5 deltas only, verified
against the tree: `refOf`/`ephemeralBit` (`arena_manifold.hpp:468-470`,
`manifold.hpp:154`), `Manifold` CellRef == MakeCell op index
(`manifold.hpp:295`, `manifold.cpp:434,442`), shadow identity
(`arena_manifold.cpp:1489-1491,1501`, `denseOf` `:255-264`), `mintSlot`
self-birthOp (`arena_manifold.cpp:1183-1187`), promote arms
(`arena_manifold.cpp:1755,1759-1764,1766,1782-1783,1797-1813,1867-1869,1947-1951`),
`isCellRelocationHandle` (`foreign_op_handle.cpp:98-117`),
`exportSymbol` (`vortex_core.cpp:119-127`), `writeValue` kind coverage
(`vortex_core.cpp:203-227`), `importValue` demote/re-mint
(`mesh_value.cpp:485-501`), resume-without-import (`mesh_actor.cpp:260-271`),
`pending`/`pumpOne`/`checkpointView` (`mesh_actor.cpp:659-662`, `:459-505`,
`:668-681`), executor seam (`mesh_actor.cpp:473-480`, `worker_context.hpp:63`),
marker precedent (`mesh_evaluator.cpp:723-735`), field/var state model
(`vagent-agent-language.md:126,390,502-507`).

## Verdicts on r5 findings

### 1. r5-vortex-1 (MAJOR — `birthOp` is not a timestamp) — RESOLVED

v1.5 deletes the mint-window conjunct wholesale and adopts exactly the ruled
replacement: "the cell's ref is a **persistent (non-`ephemeralBit`) ref at or
below the recorded program-image extent**. The discriminator is ref-space
ordering, not a timestamp: the extent is the maximum persistent `CellRef`
minted at program load, recorded once by the loader and bound by genesis step
(c) — never re-marked per genesis, since the overlay already contains every
promoted cell and a per-incarnation extent would absorb them all" (:1296-1301);
genesis (c) "bind the recorded program-image extent … recorded once and carried
as durable state … this step asserts it, it never re-derives it" (:1522-1528).

Every premise my r5 finding required to be stated is now stated and mechanically
correct:

- **Persistent refs order by append.** In `Manifold` a cell's ref *is* its
  `MakeCell` op index (`manifold.hpp:295` "== this CellRef"; `manifold.cpp:434`,
  `byRef.emplace(opIndex, dense)` `:442`), and `promote` only ever appends ops
  — so every cell promoted after program load sits strictly above the recorded
  extent. The extent therefore survives promote's renumbering *by construction*:
  the checkpoint serializes ops in append order, the resume refold gives program
  cells the same refs, and "carried as durable state" is what makes the boundary
  survive the overlay (a recomputed one would absorb the promoted cells — the
  doc's explicit non-goal, :1300-1301).
- **Arena refs always test above.** `refOf(dense) = ephemeralBit | dense`
  (`arena_manifold.hpp:468-470`; `ephemeralBit = 0x8000'0000`,
  `manifold.hpp:154`); persistent refs are op indices confined below the top bit
  by the same encoding that reserves it (R8/`isEphemeral`), so grafted/learned
  cells can never classify program-side — the installed `learn` copy reached
  through a program clause's `+d.grab` (a dim now in the vocabulary) is the
  worked example at :1304-1305, and it is correct *because* the ref arm, not
  reachability, does the excluding.
- **A shadow's base ref tests as claimed.** `shadow()` leaves `birthOp`
  untouched — "the shadow's name is the base cell's ref and not refOf(dense):
  an overlaid cell is the *same* cell" (`arena_manifold.cpp:1489-1491`), and
  `overlay_`/`denseOf` key the persistent ref (`:1501`, `:255-264`), so the
  walk sees the base ref: pre-extent ⇒ program-side "by design" (:1305-1307),
  post-extent (a promoted cell shadowed after `resume`) ⇒ state-side
  (:1307-1308). Both arms verified.
- The `birthOp` cite is gone; the changelog (:2372-2379) correctly records the
  reframing.

Residuals from my r5 text that were optional ("artifact-initial-state
consequence for captures") fold into new finding 4 below.

### 2. r5-vortex-2 (MINOR — genesis (e) walk unscoped) — RESOLVED

Genesis (e) now reads "`+d.refs` rehydration over **each `learn`-registered
head's** records — per ref-record `Int64(*handleTarget)`, per name-record the
shape-split rebind chain above" (:1530-1531), matching the step-3 boundary
claim at :1502-1506 ("an installed-but-unlearned graft's ref cells are never
re-kinded … `learn_*` is what makes a graft's references durable"). The
literal-reading contradiction is gone.

### 3. r5-vortex-3 (MINOR — "pre-promotion targets" inversion) — RESOLVED

":1536-1538 now reads "handles carrying their *promoted* targets
(`arena_manifold.cpp:1947-1951` writes `real.at(target)`)" — and the cite is
right: `store.makeOpHandle(out.version, isEphemeral(target) ? real.at(target) :
target, …)` stores the promotion-remapped persistent target
(`arena_manifold.cpp:1947-1951`).

## New findings (v1.5 deltas only)

### 1. MINOR — the program-image label→head lookup has no uniqueness rule for user `#label`s

**Location:** :1438-1443 (label arm), :1570-1577 (label shapes), :1567-1569
(duplicate refusal).

**Problem.** A name-record whose `+d.name` is a label rebinds by "matching each
fragment head's `+d.name` verbatim" over the `program()` walk. Automatic
`src:<module>:<line>:<col>` labels are unique per literal site and `reify:`
labels are serialised — but a user `code"#foo …"` overrides the label with a
bare ident (the `'#' Ident` grammar "cannot spell `:` or `"`", :1573-1574), and
**nothing requires `#foo` to be unique across the program image**. Two literals
in different modules — or the same module — carrying `#foo` both match verbatim;
the walk binds the first in skeleton order. The in-session binding was the
literal operand itself (no lookup involved), so genesis can silently bind a
*different* head than the one the session ran — stable across incarnations
(same walk order every resume), wrong forever. The only duplicate-label refusal
in the doc is `d.code`-registration-scoped (:1567-1569); it does not cover
program-image literals.

**Fix:** one clause — the checker refuses duplicate `#label`s across the
program's `code"…"` literals (the SourceLocation-keyed fragment table at
:1990-1991 already enumerates them), or `src:`-style labels become the only
re-resolution key and user `#label`s are provenance-only. Given the verbatim
match, uniqueness is the cheap invariant.

### 2. MINOR — the `reflect`-born `run`-operand arm has no `d.name` to record

**Location:** :1409-1410 ("a `code_named`/`reflect`-born operand → its `d.name`
label").

**Problem.** Reflect wrapper heads carry `+d.entry` "like every head" and are
memoized by a *record's* `+d.name` — the memo-kind label `"test"`/`"body"`
(M8 :284-291, §6.6 :1334-1341). No `d.name` on the wrapper head is defined
anywhere; the memo-kind label is not a per-head re-resolution key (every
`test_of` memo carries `"test"`). So the name-record's `+d.name` has no
referent for the reflect arm — the record cannot be written as specified. The
arm is also suspect on its own terms: a `reflect`-family result is *opaque* by
the doc's own rule (:423, :692), so "statically-known" can attach at most to
the argument expression (e.g. `test_of(plan_named("demo:Forager.p"))`), not the
returned head — and wrappers are session-side, "they die with the program at
`resume`" (:296-298), so even a recorded label would need a defined lookup for
a class that is deliberately never re-found.

**Fix:** drop the reflect arm to the opaque-refusal rule ("a `reflect`-born
`run` operand in a learnable fragment is a closedness refusal"), or define a
`reflect:<kind>:<clause-name>` label plus a wrapper `d.name` minted to carry
it. Dropping is the honest v1 answer: `run(test_of(c))` inside a learned
fragment is a strange thing to bless.

### 3. MINOR — the `run`-operand slot's convention (entry op vs head) is under-specified where the rebind writes `entryOp`

**Location:** :1440-1443 ("→ head `+d.entry` → `writeValue(slot)`"), :1076
("`run` lowers to: read the head's `+d.entry` for the entry op").

**Problem.** The label arm resolves head → `+d.entry` → writes
`int64(entryOp)` into the `+d.ref`'d slot — uniformly with `Call` operands,
whose convention is explicit (:1421 `int64(calleeEntryOp)`). For that write to
be right, a `run`-operand slot must hold `int64(entryOp)` — i.e., the emitter
takes the `+d.entry` hop **at emission** for statically-known operands. §6.2's
lowering sentence reads like a *runtime* step ("read the head's `+d.entry` for
the entry op"); under that reading the slot names the *head*, the genesis
rebind writes the entry op into it, and the learned fragment's `run`
dereferences `+d.entry` of an op cell — a silent break on the first post-resume
activation. The intended reading is the only consistent one (opaque operands,
which would need the dynamic hop, are closedness refusals anyway), but the doc
never says the hop is emission-time for recorded operands.

**Fix:** one clause — "a `run`-operand slot holds `int64(entryOp)`: for
statically-known operands the emitter resolves the head's `+d.entry` and bakes
the entry op; the dynamic `+d.entry` hop of §6.2 remains only for opaque
operands, which `learn` refuses."

### 4. MINOR — the two-arm `closed` rule reads as refusing program-side capture links that are durable by construction

**Location:** :1380-1384 (two-arm rule) vs :137 (glossary's three classes) and
:1389 ("captures of *fields and module vars*").

**Problem.** §6.6's rule has two arms — (a) a link to a *state-side* cell, (b)
a `+d.refs` name-record — and states "no program-side reference lacks a
`+d.refs` name-record". A capture *link* to a program-side binding cell is
literally a program-side reference without a record ⇒ non-closed ⇒ refused —
which contradicts the glossary's third legal class ("a captured
field/module-var binding", :137) and :1389's blessing. The coherent resolution
is real but unstated: field/var *value* cells hang on the agent cell's/module's
`d.vars` as agent state (`vagent-agent-language.md:126,390,502-507`) and must
be state-side — program cells don't checkpoint writes (`promote` maps them to
themselves and shadow diffs mint nothing, `arena_manifold.cpp:1867-1869`) — so
captures land under arm (a). The residual edge case is a genuinely program-side
capture target (an artifact-initial binding cell, the r5 note's subject):
mechanically durable anyway (maps-to-self, and the frontier prunes below it at
`:1755`, so nothing of the skeleton is dragged), yet refused under a strict
reading. Either ruling is defensible; the doc picks neither.

**Fix:** one clause — either "a program-side capture *link* is durable by
promote's maps-to-self and needs no record" (blesses arm-(a)-plus), or
"capture slots bind state-side cells only; a program-side binding cell is
non-closed" — and if the latter, note the artifact-initial-field consequence
for :1389.

## Items verified clean in the v1.5 deltas

- **Flag protocol / wake-turn vs pump scheduling** (:1479-1494, :1507-1519):
  `pending(pid)` is `mailbox.size()` (`mesh_actor.cpp:659-662`); `pumpOne`
  processes one message per call and skips stopped/empty actors (`:459-505`);
  the executor seam hands the mutable arena to the adapter's callback
  (`:473-480`, `worker_context.hpp:63`); `checkpointView` refuses non-empty
  mailbox, `running`, `choiceDepth`/`outstandingMarks`, and in-flight
  deliveries (`:668-681`). "No turn runs between the flagged turn's end and
  `publishValue`" is a sound inter-`pumpOne` obligation; the empty-mailbox arm
  correctly *needs* the P4 wake precisely because `pumpOne` skips empty
  mailboxes; the flag's clear-only-after-publish-and-restore plus
  `ActorDown`-clears is consistent with the stop path (`:501`, `:462`). The
  P4 dependency is named in §6.6, M11 and the register row.
- **Name-record shape resolution** (:1431-1443): `exportSymbol` binds
  `sym +d.values → entryOp` (`vortex_core.cpp:119-127`), so the op-or-head hop
  is right; label fallback order (program-image walk → `code_named`'s `d.code`
  walk → fault/drop) is deterministic and `reify:`/`src:`/`#foo` label shapes
  cannot collide across the two spaces (registered heads carry only
  `reify:`-shaped labels; `#label`s can't spell `:`).
- **Sideness vocabulary** (:1290-1296): `d.extends` added, `d.dims` excluded
  with the correct reason — `promote` skips `d.dims` edges
  (`arena_manifold.cpp:1766`) and `d.dims` is generated, not stored
  (`manifold.hpp:144-148`), so the exclusion loses nothing. All listed dims
  exist in the tree.
- **Genesis ordering** (:1521-1540): (b) `ensureDimension` precedes (e)
  rehydration (dim ref-cells' promoted targets exist before `Int64` writes);
  (e) scoped to learn-registered heads precedes (f) replay; "genesis and patch
  replay alike" covers reified copies' name-records, and reify's recipe
  regenerates `+d.refs`/markers by link-tuple replay (:1040-1050).
- **`symbol_named` owner rule** (:1589-1596, §9.3 :1908): module-only owner,
  `Agent.member` in the name part, no agent arm — `symbol_named("demo","Feed")`
  matches the signature and R5-C.
- **Register row P16** (:2030): consistent with §6.6 verbatim in substance —
  P4 named as the only outside mechanism, `code_named` fallback, scoped
  genesis (e), round-trip acceptance.

## Nits (non-blocking)

- Shape-split precedence: the module-resolution test decides qualified-vs-label
  (:1574-1577, :1438-1443); a module literally named `src`/`reify`/`patch`
  would misclassify (degrades to a dropped patch, not a wrong bind). Reserve
  the three prefixes for the label arm ahead of module resolution.
- "Carried as durable state" (:1526) never names the extent's carrier
  (continuation field vs durable cell). One noun.
- State-side walk phrasing (:1308-1311): "not itself program-side" could be
  misread to include the root; the agent cell may itself be program-side — the
  root is the walk's origin, not a transit. Half a clause.
- `d.name` vs `+d.name` mixing in the vocabulary list (:1292) — notation only,
  direction is recoverable from context.

---

**Summary for the orchestrator:** all three of my r5 findings are resolved in
v1.5 — the ref-space sideness test is the ruled replacement and verifies
mechanically end-to-end (extent survives promote's renumbering because
persistent refs are append-ordered op indices; arena/shadow classification is
correct on both arms; the per-incarnation-extent hazard is explicitly refused).
New: **0 BLOCKING / 0 MAJOR / 4 MINOR / 4 nit.** The minors are all localized
to the R5-B label-lookup machinery: user-`#label` uniqueness is unenforced for
the program-image verbatim match; the `reflect`-born `run` arm names a
`d.name` no wrapper carries; the `run`-operand slot's entry-op convention is
implied not stated; and the two-arm `closed` text needs one clause on
program-side capture links. Each fix is ~1 clause; none touches viability.

VERDICT: CONVERGED
