# R13 — System Architect critique (v1.12)

Scope: layering, ownership, adapter-vs-core boundaries; verification of r12's three majors
(R12-A — `d.refFor`/`d.captured` dedicated dims; R12-B — durable verb-replay dedupe; R12-C — twin
surface registration) plus the adopted minors, and new defects in the v1.12 deltas only.
Re-verified against the tree (`arena_manifold.cpp:1353-1368` displacedIt arm,
`vortex_core.cpp:384-390` `bindInput` first-wing `+d.grab`, `store.cpp:1461-1469` `Store::scroll`
localScroll disengage, `store.cpp:3840` `opsSpool.contains`, `scroll.cpp:13-22` `scrollKey`,
`publication.cpp:1097` `historyFromSeal`'s internal `Store()` mint).

## R12-A — PARTIAL: `d.refFor` closes the slot side; `+d.captured` re-opens the identical collision at the endpoint

- **Slot side — fixed and coherent.** `d.refFor` is a dedicated correlation dim;
  `-d.refFor` on the slot is free by construction (first-position slots carry `-d.grab`, chained
  slots `-d.step`, marked slots `-d.ref` — `-d.refFor` is untouched), and the doc's own occupancy
  argument now cites the code correctly (`bindInput` writes `opcode +d.grab → slot`,
  `vortex_core.cpp:384-390`, at `:1140-1143`). The genesis (e) join is sound: ref-record
  `+d.ref → slot`, slot's `-d.refFor` backlink names the unique ref-capture record
  ("a slot pairs with at most one ref-record, so `-d.refFor` never contends", `:1144-1145`) —
  single occupancy holds on every end.
- **Minted-dim ceremony — coherent.** The new dims join every load-bearing enumeration:
  the §1 capability row (`:95`), the `ensureDimension` no-marks list (`:532-534`), the §6.1
  adapter-bindings list (`:1017-1018`) and the head-layout dim list (`:1024-1025`). "Minted only
  with no marks outstanding" survives intact — the dims are adapter-bound data written once;
  recipe records only *link* on them, and `reify` under a live mark takes `dimensionNamed` hits,
  never mints (the P17 discipline). Two stale enumerations remain — finding 3.
- **The endpoint side was not analyzed — see finding 1.** The `+d.captured → referent endpoint`
  edge lands on a shared cell: the endpoint's `-d.captured` is a *global* single-occupancy slot,
  and the doc's "shared referent named once" mitigation is chain-local. MAJOR.

## R12-B — SOUND: verb-replay dedupe closes the resurrection; ordering before (e) is necessary and sufficient

- **Mechanism.** The patch record's `alias` field doubles as the recorded superseded-target
  name — correct by construction, since the session dedupe hit is defined as the clause whose
  `+d.name` verbatim-equals the alias (`:1204-1207`, `:1213-1219`). Genesis (d) "replays each
  patch's *verb*": `learn_plan`'s replay re-matches the recorded alias against "the re-attached
  rank's clause `+d.name`s and re-unlinks the hit" (`:2150-2154`), so a dedupe-displaced declared
  clause folding back at `1..E_ops` is displaced again. The verb-replay cannot hit a learned
  twin — aliases can never spell `patch:` (`:1205-1206`) and installed copies carry only
  `patch:<m>` labels. `learn_method` rides `graft_method`'s own unlink-first; `learn_handler`
  re-appends — per-verb split consistent with the session semantics (`:1219-1221`, `:1748-1750`).
- **Ordinal replay makes the re-match deterministic.** At (d), rank contents = image clauses
  plus installs from patches `< k` — the same sequence the session saw, minus session-only
  grafts. The in-session "dead until genesis" rule for the displaced clause is now stated
  (`:1221-1223`: returns at next genesis only if the displacing patch was unlearned first).
- **Ordering before the `+d.refs` sweep (e) is sound and required.** The re-unlink removes an
  image-rank member that carries no ref-records, so (e)'s scope (registered heads + (d)'s
  install set) is unaffected; conversely (e) *needs* post-(d) state — a name-record rebind must
  see post-installation and post-displacement symbols, which is exactly the stated reason (e)
  runs last (`:2191-2195`). The new "(d) hands (e) its install set" hand-off
  (`:2157-2162`) is internally coherent — replayed copies hang off pruned program-side anchors
  no head-only traversal reaches.
- **Residual:** the replay's dedupe victim can differ from the session's — finding 4 (MINOR).

## R12-C — FIXED: the segment-apply surface is registered

§10.2's preamble now enumerates the adapter-side owed surfaces `(i)–(iv)` with the twin's
segment-apply surface as `(iii)` (`:2787-2791`), and a post-register paragraph registers it
outside the core table with the honest conditionality — owed only if the kept-in-step twin is
kept, the `base()->store()` verify arm standing alone (`:2805-2810`). The `:1590-1594` forward
reference now lands somewhere real. **Every named seam has a registered owner** — except one
arguable bundle, finding 5.

## Adopted minors — all verified

- Agent-cell `d.vars` pin: "the *agent cell's* `d.vars` rank" (`:1179`), "`d.vars` serial"
  (`:1111`); the glossary patch row carries it too (`:136`).
- Re-mint transport: "**re-mint**: a fresh label cell written with the record's text exactly,
  never a re-link … which single occupancy would evict off the record" (`:2143-2146`) — two
  equal-text `patch:<n>` cells per clause-installing patch (`:1742-1743`).
- `≤E_ops` walk scoped: "named `>E_ops` legs hanging off the *agent cell* are not reached by
  this walk at all" (`:2082-2087`) — the two-root split named.
- `(d)` "no serial consumed" scoped to `patch:<n>`; the install copy's `reify:<src>:<serial>`
  still mints off the shared counter (`:2138-2140`) — gaps legal, monotone preserved.
- Per-verb `+d.name`/serial split (`:1738-1748`): `learn_where`/`learn_body` install nothing
  (label on the record alone); `learn_method`'s symbol keeps functional `owner.name`.
- `unlearn` `d.patch`-record refusal by `-d.patch` backlink (`:1794-1797`); alias→clause join
  spelled (`:1191-1194`); dedupe scope pinned to the install goal's own `d.clause` rank
  (`:1201-1204`); self-contradiction corrected — "the match is *any* verbatim hit: declared or
  grafted, qualified or unqualified" (`:1204-1205`); `d.weight` de-ranked (`:1495-1496`).
- Citation nits spot-verified: `Store::scroll` nullopt on `localScroll` (`store.cpp:1462-1469`),
  `opsSpool.contains` at `:3840`, `scrollKey` at `scroll.cpp:13-22`.

## New findings (v1.12 deltas only)

### 1. MAJOR — `+d.captured → endpoint` is single-occupancy *at the endpoint*: every shared referent across chains evicts a live recipe edge — the recipe is read-once for external referents (§6.1 `:1145-1158`, `:1050-1057`; §6.6 `:2163-2168`)

**Problem.** The R12-A fix analyzed single occupancy at the *slot* (`-d.refFor` free by
construction) but applied the same "dedicated dim" move to the *endpoint* without noticing the
endpoint can be **shared**. A state-side referent's `-d.captured` holds exactly one edge,
globally — `link` clears the incumbent (`displacedIt` arm,
`arena_manifold.cpp:1353-1368`, the same mechanism the doc invokes at `:1138-1143` and
`:1064-1066`). The doc itself concedes the collision class — "two records naming one referent
collide on the same single-occupancy slot" (`:1148-1149`) — but the mitigation is chain-local:
"later records carrying `+d.captured → the earlier ref-capture record` (a recipe-chain member,
discriminated by its chain membership since no referent sits on this head's recipe chain)"
(`:1150-1152`). That mitigation cannot extend across chains on either arm:

- **Across heads:** two `code"…"` literals in one scope capturing the same member var/field —
  the flagship capture shape (§5.3 "the cell *is* the variable"; §8's `w` example) — each emit
  `record +d.captured → S`. Head B's emission *evicts* head A's recipe edge at emission time.
  A's recipe then has a ref-capture record with no endpoint — discovered only at the next
  `reify`/genesis, which writes a stale literal or an unbound slot. Silently.
- **Reify consumes its own source:** "the copy's capture links point at the same
  definition-site cells" (`:1161-1162`) and the copy's recipe is regenerated "endpoints
  remapped" (`:1163-1164`) — external endpoints verbatim → `copy's record +d.captured → S`,
  evicting the *source head's* edge the moment the copy is written. Since `learn_*` reifys
  twice (`:1473-1475`), the second reify's recipe evicts the first's; genesis (d)'s replayed
  copy evicts the registered head's surviving edge *during the same pass that read it*. The
  registered recipe is therefore **read-once for external referents**: it survives exactly one
  reify/resume, and the *second* genesis reads a headless record → the durable patch's
  installed copy misbinds or fails.
- **The cross-chain escape is foreclosed by the stated discriminator.** Routing head B's record
  at head A's record fails twice: `R_A`'s `-d.captured` is itself single-occupancy, and the
  replay discriminator — "is the `+d.captured` target a member of *this* head's recipe chain" —
  would classify `R_A` (on B's chain? no) as *the endpoint itself*, writing `int64(R_A)` into
  the slot.
- **Within one chain the "named once" rule is still under-specified for ≥3 records:** "the
  earlier ref-capture record" reads as the original endpoint-holder (then a third record's
  `→R1` evicts `R2`'s edge — same collision one cell over) or the immediately-prior record
  (then the endpoint is `n` hops away, contradicting genesis (e)'s "one hop on a shared
  referent", `:2166`). Three slots in one fragment capturing the same cell is routine.

**Why internal referents are safe — which points at the fix.** An *internal* referent's
`+d.captured` names a cell inside the head's own graph, and a copy's record names the *copy's
twin* via the old→new map (`:2182-2183`) — fresh cells, never shared, `-d.captured` never
contends. The collision is specific to **external/state-side referents**, and that arm needs
no edge at all: the endpoint is recoverable from the slot the record already names. In-session
the source slot's `int64` literally is `S`'s position; post-receive the marked slot's
`*handleTarget` is `S`'s promoted position — the identical value (e) already writes, per the
doc's own equality ("writes the identical bits `*handleTarget` on the donor slot names, an
equality that holds for the external arm only", `:2183-2185`). The slot *is* the external
endpoint record.

**Fix.** (a) Preferred: scope `+d.captured` to *internal* referents only — per-head unique, no
sharing, `-d.captured` provably uncontended. External/state-side referents carry a shape flag
on the ref-capture record ("external: endpoint rides the `+d.refFor`-named slot's own content —
`int64` in-session, `*handleTarget` after `receiveValue`") and no edge; program-side keeps
`+d.name`. This deletes the "named once" machinery entirely. (b) If a uniform endpoint edge is
preferred, the endpoint must be rank-ed: `S +d.capRef → anchor₁`, anchors chained `+d.step`,
each record `+d.captured → anchorᵢ` — more cells and dims, house-style, and the discriminator
must become record-shape (`+d.refFor` present), not chain membership. Either way, state the
3+-record rule unambiguously.

### 2. MINOR — two dim enumerations were not updated with `d.refFor`/`d.captured` (§10.1 `:2745-2747`; §14 R12 row `:2983`)

The §10.1 adapter step's binding list ends "`d.recipe/d.mesh.relocation`" (`:2746-2747`) and the
§14 R12 invariant row likewise lists through `d.recipe`, `d.mesh.relocation` only (`:2982-2983`) —
both predate the two new dims. Harmless mechanically (`ensureDimension` find-or-mints at genesis
(b)), but an implementer binding the V10 dims off §10.1's checklist ships a shorter list than
§6.1's `:1017-1018`. **Fix:** append `d.refFor`/`d.captured` at both sites.

### 3. MINOR — replay dedupe can displace a different clause than the session displaced; the divergence rule is stated only for the faithful case (§6.1 `:1213-1223`; §6.6 `:2150-2154`)

The re-match re-derives the victim from the canonical rank, not from the recorded target — so
(a) if the session dedupe hit a *grafted* twin while the declared same-named clause stayed live,
the replay (no grafts exist) unlinks the **declared** clause — a more significant victim than the
session's; (b) if the displacing context changed — an earlier patch whose unlink fed this one's
no-hit append is *dropped* at replay (dead-target fault) — a session append replays as a
displace. Both are deterministic and arguably canonical-state-correct, but "displaced again
rather than resurrecting" (`:2152-2153`) reads as replay-faithful and neither arm is named.
**Fix:** one clause: "replay re-derives the dedupe victim from the folded rank; it is the
verbatim alias match at replay time, which need not be the clause the session unlinked."

### 4. MINOR — the `documentId_`-adoption overload is named owed but absent from §10.2's `(i)–(iv)` register (`:1536-1540`, `:2722-2727` vs `:2785-2792`)

`:1539-1540` owes "a `historyFromSeal` overload or a `restorePublication` seed alongside the
supply-side seam" and §10.1 repeats "owed with the supply-side seam" (`:2726-2727`) — but the
§10.2 enumeration `(i)–(iv)` names only base-injection, take-or-verify, segment-apply, and P4's
wake (`:2785-2792`). "Alongside" is ambiguous between *part of* `(ii)` and a fifth surface;
with `(iii)`'s registration now showing the pattern, the id-adoption arm should be enumerated
or explicitly folded. **Fix:** add `(v)` or extend `(ii)`'s text to cover it.

## Nits

- `:1187` "the alias is write-only audit metadata" — now read by genesis (d) replay; the
  sentence's own "two uses" clause names replay dedupe, so "write-only" is stale (say
  "replay-keyed, never a session lookup key", matching `:1217`).
- `:2166` "(one hop on a shared referent)" — the hop count depends on the chain reading finding
  1 forces; restate with the fix.

## Verified v1.12 deltas (no finding)

- **R12-D comparand harmonization:** (a) `1..E_ops` order-vs-order — non-scroll fields verbatim
  plus scroll *identity* by `scrollKey` with the `localScroll` disengage named
  (`:2056-2064`; `Store::scroll` nullopt arm `store.cpp:1462-1469` verified) — and §6.6's verify
  arm now cites "(a)'s stated comparand, applied verbatim" (`:1585-1587`). One canonical
  statement, one cite. Fixed.
- **`learn_method` replay-via-verb:** `graft_method`'s unlink-first is part of the grafted verb
  (`:1219-1220`) — replayed unlink-first displaces the folded declared symbol identically.
- **(e)'s ref-record read** now derives the endpoint through the `+d.refFor`/`+d.captured`/`+d.name`
  triple (`:2163-2168`) — the *pairing* half is sound; the endpoint half is finding 1's subject.
- **Two `patch:<n>` label cells** (record + installed copy) resolve the shared-label occupancy
  cleanly — `+d.name` single-link verified.

## Structural notes

- The recipe's durability model is now visibly *one-deep*: records are durable cells, but any
  edge landing on a shared external cell inherits that cell's global occupancy. Finding 1 is
  the third round the same M14 mechanism has surfaced one cell over (`+d.ref`, then `+d.grab`,
  now `+d.captured`) — the pattern suggests the design should adopt a stated rule: **no recipe
  edge lands on a cell the recipe does not own unless that edge is provably the only claimant.**
- The `(d)→(e)` install-set hand-off is the right layering — genesis-internal sequencing, no
  new surface. Verb-replay dedupe keeps dedupe logic in one place (the verb), not two (session
  + a replay-only re-unlink table).

VERDICT: NOT CONVERGED
