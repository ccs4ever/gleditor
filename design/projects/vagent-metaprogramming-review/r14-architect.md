# R14 — System Architect critique (v1.13)

Scope: layering/ownership/adapter boundaries; verification of R13-A (`d.captured` dropped,
endpoint carried as content), the adopted minors (dim enumerations :2788/:3028, replay dedupe
divergence :1230-1237/:2188-2190, §10.2 (v) `documentId` arm :2833-2837), and new defects in the
v1.13 deltas only. Re-verified against the tree (`publication.cpp:853-869` `globalKeyOf`,
`publication.hpp:447` `breakMarkerKey`, `publication.cpp:985-987` sealableOps skip, `:1049-1050`
identity remap, `store.cpp:1461-1464` `Store::scroll` disengage on `localScroll`/out-of-`externals`,
`store.cpp:123-135` `Store()` id-less vs 2-arg `documentId` ctor, `publication.cpp:1097`
`historyFromSeal` `make_unique<Store>()`, `publication.cpp:1113-1128` `restorePublication` passing
`pub.storeId`, `zigzag/arena_manifold.cpp:1950` `isEphemeral(target) ? real.at(target) : target`,
`vortex_stdlib.cpp:1953` `d.vars` walk matching `textOf`).

## R13-A — FIXED, verbatim, and architecturally closed

The fix was adopted as ruled: `d.captured` is gone from every load-bearing site — §1 row `:95`,
the `ensureDimension` no-marks list `:533`, the §6.1 adapter bindings `:1017-1018`, the head-layout
intro `:1024-1025`, the diagram `:1050-1059`, the prose `:1147-1167`, genesis (e) `:2200-2234`,
§10.1 `:2788`, §14 R12 `:3028`. The only surviving mentions are the change-history paragraphs
(`:3510-3513`, `:3548-3560`) — correct.

**The shared-referent cases all close, by deletion of the channel rather than another relocation:**

- **Star ≥3 (one recipe, one external referent):** three slots each hold `int64(S)` as *content*;
  content has no single-occupancy slot to contend. Three `+d.refFor`-paired records regenerate the
  same endpoint independently — nothing to evict.
- **Chains:** the "named once / one hop" machinery is deleted outright; no chain reading remains to
  mis-specify.
- **Cross-head / sibling `reify` copies:** a copy's regenerated slot carries `int64(S)` verbatim —
  no edge lands on `S`, so the copy cannot sever the source head's anything. The v1.12 self-eviction
  channel (record `+d.captured →` shared `-d.captured`) is gone.
- **Internal referents:** the optional `+d.values` on the record is occupancy-gated at link time
  (`:1155-1157`) and is never load-bearing — the slot's `int64` rebind through the old→new map is
  authoritative at replay (`:2221-2223`), `closed` reads content (`:1166-1167`). A later legitimate
  `+d.values` claim on the same endpoint can only *evict the decoration*, never the carrier. The
  discriminator is now a fact — map membership — not a chain-membership heuristic.
- **Program-side:** `+d.name →` a fresh name cell per record (`:1161-1165`); two records naming one
  dim mint equal-text cells — no shared-cell contention. The program-skeleton-edge violation is
  still refused.

The design now satisfies the rule r13 proposed: **no recipe edge lands on a cell the recipe does
not own** — every surviving recipe edge (`+d.refFor`, the tuples' `+d.values`, the gated internal
`+d.values`, `+d.name` to a fresh name cell) terminates inside the head's own graph. The M14
mechanism finally has no seam left to relocate to.

## Adopted minors — all verified

- **Dim enumerations** — `:2788` (§10.1 checklist) and `:3028` (§14 R12 row) both carry `d.refFor`
  and correctly drop `d.captured`; consistent with §1 `:95`, `:533`, `:1017-1018`, `:1024-1025`.
- **Replay dedupe divergence, both cases** — `:1233-1237` states the faithful-and-divergent rule
  verbatim ("the verbatim alias match *at replay time*, which need not be the clause the session
  unlinked — a session hit on a grafted twin reduces to the append case … a session append replays
  as a displace when an intervening displacer's patch was replay-dropped"); `:2188-2190` repeats it
  at genesis with the back-reference. Complete.
- **§10.2 (v) `documentId`-adoption arm** — `:2833-2837` registers it with correct citations:
  `historyFromSeal` mints `Store()` with no id (`publication.cpp:1097`; the zero-arg ctor
  `store.cpp:123-125`), while `restorePublication`'s existing two-arg mint passes `pub.storeId`
  (`publication.cpp:1127-1128`) — both named fix paths are real. **The register is now complete:
  every seam named anywhere in the doc is owned** — (i) spawn base-injection, (ii) `resume`
  take-or-verify, (iii) segment-apply (honestly conditional on the kept twin), (iv) P4's approved
  wake, (v) `documentId` adoption; P17 deferred-not-registered; the call ABI/`staticFuel` items are
  vagent's own debt or new code, correctly outside the register.
- **Comparand `globalKeyOf` arm set** (`:1580-1590`, `:2083-2097`): minus-`localScroll` is justified
  (`remap.emplace(localScroll, selfScrollInHistory)` `:1049` — decoded records never carry it);
  `breakMarkerScroll` compares by `breakMarkerKey` (skip-not-reject `:985-987`, identity-remap
  `:1050` — verified verbatim); out-of-`externals` disengagement fails the compare — fail-closed,
  correct.
- **Alias wording** — "no session resolver consults the alias — the `d.patch` rediscovery join
  below and `learn_plan`'s replay dedupe do" (`:1196-1197`); conditional presence stated
  (`:1225-1228`, `:1742-1745`); join multiplicity + latest-wins-by-`ordinal` (`:1201-1205`).
- **`patch:<n>` equal-text labels** (`:1738-1743`), label-cell `-d.name` direction (`:1763-1764`),
  `label_of`/genesis-transport vs `unlearn`-never-matches split (`:1761-1763`, `:1767-1768`),
  `textOf`-content claim (`:1764-1767`; cite verified), `Patrol`/`demo:Guard.cautious` premise
  (`:1218-1219`), comparand reference-side phrasing (`:1598-1610`, `:1984-1987`), §9.3 else-seed
  (`:2651` + `:2448-2449`).

## New findings (v1.13 deltas only)

### 1. MINOR — `closed`'s content-read has no program-side carve-out; a literal reading refuses every `dimConstant`-bearing fragment (§6.1 `:1166-1167`; `closed` spec `:1839-1864`)

"The frontier stays enumerable — `closed` reads each ref-capture endpoint through the paired slot's
own content" — but for a `+d.name`-bearing *program-side* record the slot's content is an
arena-local **dim id**, not a cell position. Read as a position it lands on an arbitrary
program-band cell → "a program-side reference with no record" → the refused class on both arms
(`:1846-1848`) → `learn`'s re-verification rejects a legitimately name-resolvable fragment. This
bites universally: `dimConstant` emission mints a ref cell for *every* `cells"…"` path dimension
operand (`:2005-2007`). v1.12 had no such misread — the `+d.name` arm had no endpoint edge at all;
v1.13 hands `closed` a bogus position unless it checks `+d.name` first. Genesis (e) already uses
the right discriminator (`+d.name` → `ensureDimension` name arm, `:2204-2206`) — `closed`'s consumer
sentence needs the same arm stated. **Fix:** append "— a `+d.name`-bearing (program-side) record's
endpoint is the named dim, not a cell, and counts as name-resolved" to `:1166-1167` (and align
`:1863-1864`'s frontier enumeration to say the same).

### 2. MINOR — the revised agent-cell-legs sentence names the mechanism this same revision deleted (§6.6 `:2114-2121`)

"the walk reaches them through capture edges — a fragment's slot link to an agent-`d.vars` binding
cell" — under v1.13 a state-side referent takes **no edge at all** (`:1158-1160`); the only edge
that ever ran program-reached-structure → agent-`d.vars` cell was the deleted `+d.captured` on
installed copies' recipe records. Under the content rule no program-rooted `+d` path reaches an
agent-cell leg — surviving beyond-band reaches land on floating session cells (installed copies via
`+d.grab`, `learn_method` symbols via `+d.values`), not on agent-rank legs. The hedge is vacuously
true and the conclusion (no agent-rooted second walk owed) is a fortiori — but the cited example is
stale. **Fix:** re-point the example to an install/memo edge, or restore the honest v1.12 shape:
"capture endpoints now ride slot *content*, so no capture edge reaches the agent cell at all — the
beyond-band legs the walk can reach are install-side cells, all `>E_ops`."

### 3. MINOR — ":1170-1171 the copy's capture links point at the same definition-site cells" is doubly stale

(i) External endpoints now take no link — the copy's *slot content* names the same cells; (ii)
internal endpoints name the copy's *twins* via the old→new map, not "the same definition-site
cells". The following claim ("`closed` iff the source was") remains true. **Fix:** "the copy's
capture slots name the same definition-site cells for external referents, and their own twins for
internal".

### 4. MINOR — `set_weight`'s new mint arm has an unnamed failure mode on nameless clauses (§8.1 `:2448-2449`)

The mint is keyed by `clause → +d.name → qualified string` — declared handlers carry no `+d.name`
(`:1098`), so `set_weight` on one has no key to mint under; `weight_of` disengages cleanly but the
write arm's refusal/disengage is unstated. Relatedly, records keyed by `patch:<n>` outlive their
clause after `unlearn` and accumulate on agent `d.vars` with no compaction story — widened, not
created, by the mint arm. **Fix:** one clause each — "a `+d.name`-less target fails the statement
like any `Opt` disengage" and "weight records keyed by a dead label are garbage, reclaimed by the
same dead-patch drop idiom or left documented."

## Nits

- `:3555-3556` (changelog): "external/state-side **and program-side** endpoints ride the slot's own
  `int64` … `*handleTarget` … (what `promote` already translates)" misdescribes the adopted arm —
  program-side endpoints ride `+d.name` → name re-resolution (`:1161-1165`), and `promote` does
  *not* translate dim ids (`isEphemeral` guard, `arena_manifold.cpp:1950` → verbatim). History text
  should match §6.1.
- `:1155-1157`'s occupancy-gated `+d.values` makes the ref-capture record's shape
  emission-order-dependent; stated, deterministic, non-load-bearing — worth one "the recipe reader
  keys ref-capture records by `+d.refFor`, never by `+d.values` presence" clause if a future reader
  walks the chain generically.

## Structural note

The v1.13 delta is the first revision in this review series whose net effect on the recipe graph
is *fewer* edge classes — every prior M14 patch added a dedicated dim (`+d.ref` → `+d.refFor` →
`+d.captured`); this one deleted the dim and let content carry it. That is the house-style move
(more cells, no bit-packing — and no single-occupancy edge where content suffices), and it
eliminates the collision *class* for external referents rather than moving it one cell over.

VERDICT: CONVERGED
