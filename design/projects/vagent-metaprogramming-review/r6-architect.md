# R6 — System Architect critique (v1.5)

Scope: layering, ownership, adapter-vs-core boundaries, Makefile/dependency hygiene; verdicts on my
four r5 findings plus new defects in the v1.5 deltas only (`:1289-1318` extent test, `:1424-1471`
name-record/flag phases, `:1521-1540` genesis ordering, `:1896-1937` examples/inventory region,
`:1957-2030` §10 inventory + P16 row, `:352-357` M11). Re-verified against the tree
(`apps/common/xanadu/{vortex,zigzag,vagent}/`, `Makefile`, `design/vagent-agent-language.md`).

v1.5 lands the r5 batch faithfully: the `birthOp` test is gone, replaced by ref-space ordering with a
loader-recorded extent; the flag gained the no-turn window and reconciled clear semantics; P4 is
named in the register row, M11 and §6.6; "pre-promotion" is fixed; the name-record rebind is the
adopted shape-split design. All four of my r5 findings are resolved in substance. The residual work
concentrates — as the disposition predicted — in the newest machinery: the recorded extent itself.

## Verdicts on my r5 findings

### 1. MINOR (wake-era scheduling: a queued wake could fire the restore before `publishValue`; flag-clear ambiguous) — RESOLVED

`:1486-1488` now states the invariant flatly: "**No turn — message or P4 wake — runs between the
flagged turn's end and `publishValue`**", and `:1507-1509` pins "the next turn" as "the first turn
after `publishValue` returns". The clear/restore ambiguity is reconciled: the flag "clears only once
publish has succeeded *and* the head-of-turn restore has run — the restore itself is unconditional
on marked cells still in `OpHandle` kind and runs on every exit path, so a `publishValue` failure or
refusal still restores while the flag stays armed for the next attempt, and `ActorDown` clears it
outright" (`:1489-1494`). The "two-phase" nit is also fixed — "a three-state marker" (`:1488`).
One residual wording gap → new nit (the stated justification covers only checkpoint-minted wakes).

### 2. MINOR (the empty-mailbox arm's P4 dependency unnamed in the "no new core mechanism" claim) — RESOLVED

All three sites now name it. The P16 row reads "an **adapter-side ceremony needing no mechanism
beyond P4's already-approved wake**" and its Where column says "the adapter's own executor TU **plus
P4's approved `mesh_actor` wake change**; the rest reuses `foreign_op_handle`/`publishValue`/
`checkpointView` as-is" (`:2030`). M11 carries the same wording (`:353-355`), and §6.6 says it twice
more — the ceremony header (`:1455-1457`) and the arming step's "the only part of this ceremony
outside current code" (`:1484-1486`). The dependency is now declared everywhere a triager would
look.

### 3. MINOR (genesis receives handles carrying *promoted* targets, not "pre-promotion") — RESOLVED

`:1535-1538` now reads "receives the `OpHandle`-kind cells verbatim — handles carrying their
*promoted* targets (`arena_manifold.cpp:1947-1951` writes `real.at(target)`)" — verified:
`makeOpHandle(out.version, isEphemeral(target) ? real.at(target) : target)` at
`arena_manifold.cpp:1949-1950`. The modifier no longer contradicts the rehydration clause.

### 4. MINOR (`birthOp` mint window: boundary provenance unstated; per-genesis re-marking contradicts the test) — RESOLVED via the R5-A reframing

The disposition's adopted redesign is in place: sideness is reachability **and** "a persistent
(non-`ephemeralBit`) ref at or below the recorded program-image extent" (`:1296-1297`); the extent
is "recorded once by the loader and bound by genesis step (c) — never re-marked per genesis, since
the overlay already contains every promoted cell and a per-incarnation extent would absorb them all"
(`:1298-1301`), restated at `(c)` as "recorded once and carried as durable state … this step asserts
it, it never re-derives it" (`:1524-1528`). My clause (a) — recorded once, never re-based to the
post-overlay extent — is verbatim. Clause (b) — my proposed "non-ephemeral `birthOp` is outside the
window" arm — was superseded by the disposition's better ruling: shadows classify program-side *by
design* since their base ref is pre-extent and persistent (`:1305-1308`), which is coherent with the
ref-space framing and states the ordering for the shadow case outright. Ephemeral refs
(`refOf` = `ephemeralBit | dense`, `arena_manifold.hpp:468-470` — verified) always test above
(`:1301-1304`). The provenance gap I flagged is closed in *rule* — but the construction the rule
presumes is not → new finding 1.

## New findings

### 1. MAJOR — the `≤ extent` discriminator presumes a per-incarnation persistent ref layout that no step sequences (§6.6 `:1297-1308`; genesis `:1521-1540`; `:1548`)

**Problem.** The boundary only discriminates if the persistent ref space at every incarnation is
laid out **[program image `0..E`] below [promoted/checkpointed state `>E`]**. The text asserts the
consequence — "a promoted cell's post-extent persistent ref keeps it state-side across `resume`"
(`:1308`) — but never states the construction that produces it, and the pieces it does state point
the other way:

- The program image is **not** in the checkpoint: program cells "do not checkpoint" (M11 `:344`),
  and genesis "restores the program's own attachment for free because `programRef` **reloads the
  skeleton**" (`:1548`) — i.e., the image is re-materialized per incarnation, not carried.
- The checkpointed manifold exists **before** genesis: `mesh_actor.cpp:260-271` shows
  `receiveValue` → `store->rebuildManifold(version)` (`mesh_value.cpp:350-351`) →
  `WorkerContext(actor->checkpoint->manifold.get())` built at actor construction, with the arena
  overlaid on it. The received manifold already contains the promoted state cells — at whatever
  positions the rebuild assigned — before any genesis step runs.
- Genesis's fixed ordering `(a) arena overlay → (c) extent → (d) bind programRef → …` never names
  where or when the image re-materializes relative to that received state. A `Manifold` is flat
  (no `base_`; `makeCell` appends), so the natural implementation — reload the artifact into the
  received manifold at (c)/(d) — lands **every program ref above E**, silently inverting the test:
  program cells classify state-side, `d.code`'s invariant is vacuous, `learn` may register
  `code"…"` originals, and the checkpoint drags the skeleton — V12 verbatim, silently.
  The mirror-image failure is equally available: if the checkpoint's rebuild re-densifies (state
  ops replay into a fresh manifold at positions `0..k`), every durable cell tests `≤ E` and
  classifies program-side — the transit clause then prunes all of it and `learn`'s re-verify
  rejects its own registered heads.
- "Never re-derives it" (`:1527`) forbids the one escape hatch — re-observing the high-water after
  image materialization.

The only self-consistent construction is a two-phase materialization — the image's ops occupying the
low persistent positions *before* the checkpointed state's cells join the space (deterministic
artifact replay into a store the checkpoint's cells then extend, or an equivalent layering) — and
that ordering constraint is exactly the kind of thing a "fixed ordering" step list exists to pin.
It is pinned for `(a)–(g)` and absent for the step the new discriminator depends on.

**Fix.** One clause at (a) or before it: the persistent space is constructed image-first — e.g.,
"the loader materializes the artifact into the persistent manifold before the incarnation's
checkpointed state joins it; persistent positions are stable across `rebuildManifold`, so image
cells hold `0..E` at every genesis and promoted cells always land above the boundary." If positions
are instead preserved by the carried store's absolute op indices (`birthOp == CellRef`,
`manifold.hpp:295`), say *that* — either way the ordering source must be named.

### 2. MINOR — the extent's carrier, bound form and owner are all unnamed (§6.6 `:1525-1528`; §10.1 `:2004-2008`)

**Problem.** "Recorded once and carried as durable state" never says whose state: an adapter field
dies with the process and a cold resume would have to re-observe — contradicting "never re-derives";
a checkpointed state-side scalar (the `d.vars` serial precedent, `:1578-1579`) needs a named
cell/dim home. Nor is the bound *form* named: `(c)` "binds" it, presumably by the V10 mechanism like
`(d)`'s `programRef`, but no dim/`d.vars` key is given — and it matters who consumes it, because
`learn`'s closed re-verification (`:1404-1407`) must evaluate `ref ≤ E`: if `learn` is a std:meta
routine the extent must be readable and orderable from Vortex code, if native the adapter owns the
comparison — unstated either way. Finally, "this step asserts it" (`:1527`) has no referent: an
assert needs something observed to compare against, which is precisely the post-image high-water
finding 1 says must be constructible. Compounding it: §10.1's inventory assigns the recording duty
to no one — the loader is the base spec's `program.{hpp,cpp}` (new code, `vagent-agent-language.md`
§11.1) but gains no listed duty, and the adapter row's bind list names no extent binding.

**Fix.** Name the residence (a `d.vars` scalar on the agent cell is the precedented choice), the
bound spelling `learn`/genesis read, and a one-line `program.{hpp,cpp}` inventory duty: "records the
program-image extent (post-load persistent `cellCount`) for genesis (c) to bind."

### 3. MINOR — a `reflect`-born `run` operand gets a `d.name` record whose rebind is guaranteed dead (§6.6 `:1409-1411`)

**Problem.** The emitter "name-records every statically-known `run` operand — a literal operand →
its `src:` label, a `code_named`/`reflect`-born operand → its `d.name` label". But a `reflect`-born
head is an M8 wrapper: session-side, unpromotable, never on `d.code` (`:1334-1336`), and spec'd to
carry `+d.entry`, not a `+d.name` label. If it carries no label the record is unwritable; if it
carries one, the genesis rebind must fail — the wrapper is in neither the program image (the
label→head walk's domain) nor `d.code` (`code_named`'s) — so a *learnable* fragment containing
`run(wrapper)` is accepted at emission and produces a patch whose name-record rebind necessarily
drops at genesis (`:1443-1444`). The failure is loud, not silent, but it is a guaranteed-dead
contract at the emission↔genesis seam: a session-only referent cannot be made durable by naming it.

**Fix.** One clause: reflect-born `run` operands join the opaque-target arm — a session-side wrapper
is not a durable referent, so it is a closedness refusal at emission rather than a name-record.

### Nits

- **`patch:` is a dead shape in the label→head arm** (`:1438-1439`). `patch:` labels name patch
  records (the patch cell's `+d.name`, `:1351`), not fragment heads; any head carrying a patch-side
  label is state-side and reached through the `code_named` fallback, not the program-image walk.
  Either state what a `patch:`-shaped record's head is, or drop the prefix from this arm.
- **The no-turn window's stated justification is narrower than the rule** (`:1486-1488`). "The
  publish path enqueues its wake turns itself" covers the checkpoint-minted wake, but a
  ready-intention or due-timer wake queued under `vagent.ready_per_message`
  (`vagent-agent-language.md:1638-1653`) *before* the flagged turn's end is also "a P4 wake turn"
  the rule must hold behind publish. The flat clause covers it; the cited mechanism does not —
  saying "the pump orders publish ahead of all queued turns while the flag is armed" closes it.
- **`d.name`/`+d.name` direction spelling** (`:1409`, `:1292-1293`). The rebind text says heads are
  matched on "`+d.name`" while the emitter records "its `d.name` label", and the sideness
  vocabulary lists both `+d.name` and `d.name` under an explicitly *outbound-following* walk —
  pick one convention (or state that `d.name` in the list means the dim's negward edge is also
  walked, contradicting "outbound").

## Structural notes (no finding)

- **The label→head lookup is honestly adapter-placed.** It is "the same outbound `program()`
  structural walk the sideness test runs" (`:1440-1441`), run inside genesis step (e) — after (d)
  binds `programRef`, so the root exists when the walk needs it — and genesis runs outside turns,
  where only the adapter (mutable `WorkerContext` arena) can execute a graph walk; `std:meta`
  builtins cannot run between turns. The pre-existing dual-layer shape — adapter genesis walks vs
  `code_named`/`symbol_named`/`plan_named` as in-turn builtins — is unchanged by this delta.
- **The loader→adapter datum is not a new core coupling.** "The loader" is vagent's own
  `program.{hpp,cpp}` (`vagent-agent-language.md` §11.1, established at `:280` as "the artifact
  loader" that already binds the program root) — new code reading `cellCount`-class manifold state.
  The seam is intra-vagent; the defect is only that the duty/carrier is unassigned (finding 2).
- **"No new mechanism beyond P4's already-approved wake" still holds** — for *existing-code*
  changes. The extent record, the `program()` structural walk, the shape-split rebind and the flag
  ceremony are all duties of new vagent/adapter code over existing APIs; P4's `mesh_actor` change
  remains the only outside-current-code dependency, and the P16 row's Where now says so (`:2030`).
- **The `+d.name` discriminator is kind-honest.** Name-records never get markers and never re-kind,
  so a program-cell target can never reach `promote`'s relocation gate (`arena_manifold.cpp:1759-
  1764`, `:1797-1809`) — the rationale at `:1424-1429` is verified.
- **Cites re-verified.** `exportSymbol` binds `sym +d.values → entryOp` with no head
  (`vortex_core.cpp:122-124`); `insertIntoRank`'s idempotence doc comment now lands at
  `arena_manifold.hpp:624-628`; `Makefile:643` (`COMMON_XANADU_SRCS` glob), `:688-690`
  (`VAGENT_FRONTEND_SRCS`), `:695` (`VAGENT_SYNTAX_OBJS`), `:698` (the `vagent/*.cpp` filter-out)
  all check out; the §10.1 Makefile line names exactly the five new frontend sources.
- **`code_named`'s domain narrowing to `d.code`-only** (`:1583-1588`) is internally consistent
  (literal heads route through the program-image walk or `run`'s record path); whether users will
  discover that split is ergo's question, not mine.
- **Step (c) binds the extent before (d) binds the root** — safe, since nothing evaluates sideness
  until (e) consumes both; ordering within the named steps is sound.

## Verdict

All four r5 findings resolved; the P4 dependency and wake-window seams are now stated precisely.
The remaining defect is the newest machinery's foundation: the `≤ extent` test is correct as a rule
but its spatial precondition — image refs below promoted state at *every* incarnation — is asserted
as consequence and never sequenced as construction, with a plausible build order that inverts it
silently. Clause-fixable but load-bearing → 1 major, 2 minors, 3 nits.

VERDICT: NOT CONVERGED — 0 blocking, 1 major, 2 minors, 3 nits
