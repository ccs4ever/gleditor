# Round-13 disposition — orchestrator rulings on v1.12 findings

0 blocking, 1 convergent major (five lanes — vortex, zigzag, architect, meta, codebase — all
independently hit it), ~15 minors. Two lanes converged (ergo, writer). R12-B's dedupe-via-verb-
replay verified sound in two lanes; R12-C/R12-D landed.

## Major — adopted

**R13-A — `+d.captured → endpoint` re-opens M14 at the shared referent's `-d.captured` slot**
(five lanes). A shared referent can hold only one `-d.captured` backlink: ≥3 same-recipe
sharers self-evict under a star reading; "one hop" is false under a chain reading; cross-head
sharing (sibling `reify` copies, later `learn_*` heads) is unencodable — every `reify`
regenerates `+d.captured →` the same external endpoint and severs the registered recipe's
edge. Routine in practice (`compiler.cpp:531-532,781,796` — shared `cells_->zero`-class
referents). **Ruling (the lanes' convergent fix):** **`d.captured` is dropped entirely** —
the capture endpoint is carried as *content*, never as an edge into the referent: external/
program-side endpoints ride the slot cell's own int64 ref position or `*handleTarget` (what
`promote` already translates); internal referents rebind through the old→new reify map
(`+d.values` on the *record* may name its own endpoint cell — one record, one edge). `d.refFor`
alone correlates record→slot. Restate §6.1's enumeration, the diagram, genesis (e), and the
minted-dim lists (drop `d.captured`, keep `d.refFor`).

## Minors — all adopted

- **Comparand disengage arm** (vortex): "(both `localScroll`, else unequal)" misfires on
  `breakMarkerScroll` (legitimately sealed, identity-remapped `publication.cpp:1050`) — use
  `globalKeyOf`'s shape for the scroll-identity check.
- **Comparand pronoun** (meta + writer): :1961 "verifies the rebuilt store's `1..E_ops` band
  **against it**" — the reference side is the decoded records, not "the adapter's seeded store";
  harmonize :1581-1584 with :2053-2055 (the seeded store IS the reference — pick one phrasing
  and keep it consistent).
- **`alias` "write-only" self-contradiction** (vortex + writer + zigzag): the same sentence
  lists two reads (replay key + rediscovery join) — restate as "no session resolver consults
  it; the replay key and the `d.patch` rediscovery join do".
- **`alias` field conditional presence** (ergo): *is* the superseded target only on a dedupe
  hit — state that; displaced-vs-appended is then auditable.
- **`label_of`/`unlearn` handle contradiction** (ergo :1740): `unlearn` matches never by the
  record's own label and refuses patch records — fix the "uniform handle" claim.
- **`demo:Forager.travel` premise** (ergo): §9.3's `demo:Forager` declares zero plans — use
  `learn_plan("Patrol","demo:Guard.cautious",…)` or state the premise.
- **Replay dedupe victim divergence** (architect): grafted-twin absorption / dropped
  intervening patch — "displaced again" must cover the faithful AND divergent cases.
- **`+d.name` direction slip** (zigzag + codebase :1743): the contended slot is the *label
  cell's* `-d.name`, not the clause's.
- **`patch:`-shadows-`d.vars` claim** (zigzag + codebase + writer): the walk matches *content*
  (`textOf`, `vortex_stdlib.cpp:1953`) — a `+d.name` edge shadows nothing; remove the claim.
- **"Agent-cell legs not reached at all"** (zigzag :2082-2084): capture edges DO reach
  agent-`d.vars` cells — conclusion right, mechanism claim wrong.
- **"Share the `patch:<n>` `+d.name`"** (meta :1721-1723): stale after the two-equal-text
  split — re-invites the `-d.name` eviction the split prevents.
- **Stale dim enumerations** (architect + writer): §10.1 :2745-2747 and §14 R12 :2982-2983
  omit `d.refFor` (and `d.captured` — now being dropped; update both lists).
- **§9.3 `on Meal` strengthen loop** (ergo, pre-existing): `if [w]` can never bootstrap a
  first `d.vars` weight record — flag as a known limitation or fix the example.
- **`store.cpp:124` confirmed correct** (codebase correction of its own r12 nit) — no action;
  the current cite stands.

## Round status

v1.13 to follow. The convergent-major pattern (five lanes, one mechanism, one convergent fix)
is the strongest signal yet that the mechanism space is nearly exhausted — the fix removes a
dimension rather than adding machinery.
