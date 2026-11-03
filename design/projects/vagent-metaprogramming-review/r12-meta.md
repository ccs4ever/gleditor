# Round 12 critique — `design/vagent-metaprogramming.md` v1.11 — Metaprogramming Expert lane

Scope: verify my r11 minors (stale `unlearn` same-name limit; `unlearn` symbol-arm
module-prefix composition; `patch:<n>` ordinal mint source); check the serial-off-`d.vars`
collision domain against `reify`'s own serial use; check the alias-write-only + rediscovery
idiom for closing learn→name→unlearn; check the dedupe consequence against quasiquote/splice
and replay semantics; check the `learn_method` qualified-success corollary (:1803-1809);
hunt new defects in the v1.11 deltas only (full diff `git diff 3f9731da..894c7f3a`
reviewed). Code citations re-verified against `apps/common/xanadu/`.

## 1. My r11 minors — all landed

- **Stale `unlearn` same-name limit — FIXED (:1181-1188 + :1734-1738).** The rewritten
  paragraph matches the suggested re-key nearly verbatim: `learn_handler` appends "under its
  own `patch:<n>`", `plan_named` by the alias "hits *neither* (zero hits, never an ambiguous
  first)", `unlearn` "picks each twin precisely by its minted label", the surviving `+d.name`
  ambiguity is "same-named **grafted** clauses only", and the LIFO-by-name is now correctly
  scoped to "patches recording one *target* name — stacked replaces on one clause, which is
  undo order, not ambiguity". The `unlearn` clause arm (:1734-1738) corroborates. All three
  stale claims are gone; nothing elsewhere in the doc reasserts them (`:1645-1647`'s "Only
  that arg-form dedupes" is the unrelated already-registered-head carve-out).
- **`unlearn` symbol-arm module-prefix composition — FIXED (:1738-1745).** "The **symbol
  arm** composes the qualified spelling: the argument's symbol cell stores bare
  `Agent.member` content (`exportSymbol` mints `makeCell(name)`,
  `vortex_core.cpp:119-127`), while the patch recorded the target's qualified
  `module:Agent.member` — ...the owning module cell reached by the `-d.vars` walk back to
  the rank head and its content supplying the `module:` prefix". Verified against the code:
  `exportSymbol` stores the name string as cell *content* (`vortex_core.cpp:122`), the
  module cell's content is the module path (`getOrCreateModule`'s `makeCell(modulePath)`,
  `:112`), and the rank is the `rankTail`/`rankAfter` same-dim chain (`:125`), so the
  `-d.vars` back-walk does reach the module cell. The mirror claim at :1716-1718 (the record
  carries the composed form, the arg carries the content form) is consistent. Works for
  goal/event symbols too — bare `Feed` content composes `demo:Feed` for the two-part
  targets.
- **`patch:<n>` ordinal → durable `d.vars` serial — FIXED (:136, :390, :1104, :1158-1161,
  :1671-1673, :1745-1747, :2068-2073, :2176-2179).** Every mint site now reads "the durable
  monotone serial drawn from the `d.vars` counter `reify` shares, never the rank position";
  :1745-1747 states ordinals are never reclaimed across `unlearn`/replay-drop; (d)
  (:2071-2073) transports the recorded `+d.name` verbatim at replay ("replay transports the
  label, it never re-derives `<n>`"); §6.7 (:2176-2179) ties `<n>` to the same counter as
  `reify`'s `<serial>`. The position-reading collision I flagged is foreclosed by
  construction.

## 2. The task's expert questions

**Shared `d.vars` counter — collision domain clean.** `reify:<src>:<serial>` (head labels,
`d.code`/image-walk space), `patch:<n>` (clause labels, `d.clause` space) and `validate`'s
per-pass tag serials (:2264-2267) all draw the one durable monotone counter — which is
*stronger* than per-space counters: a serial value is consumed once globally, so no two
mints can ever collide, and the label spaces are prefix-disjoint anyway (`patch:` refused
as a user `name` and reserved at module declaration; `#label` can't spell `:`). The only
consequence is *sparse* `patch:` ordinals — a `learn_plan` with two fragments burns ≥4
reify serials before its patch draw — and nothing depends on density: replay sorts by
recorded ordinal (:2068), `unlearn` max-matches (:1731-1734), `plan_named` matches
verbatim. The counter rides the agent's `d.vars` → state-side → promoted → survives
`resume`, which is exactly what makes it durable. One loose sentence flagged as a nit
((d)'s "no serial is consumed" is true only of the patch-ordinal draw — the replayed
`reify`s do consume serials).

**Alias-write-only + rediscovery — the learn→name→unlearn cycle closes**, mechanically:
`learn_plan` records the alias on the patch and mints the clause's `+d.name` as `patch:<n>`
→ the user re-finds the clause via `clauses_of` + the `patch:` prefix or
`plan_named("patch:<n>")` (`patch:` fails the module test, falls to verbatim label match —
the §6.7 reservation is what makes that spelling safe) → `unlearn` reverse-reads the
clause's minted label → pops the latest patch recording that target → unlinks the clause
and, on the last naming patch, removes the head's `d.code` registration (:1663-1664). One
gap: the alias→ordinal *join* is never spelled — MINOR 3.

**Dedupe consequence vs quasiquote/splice — the content layer is untouched; the replay arm
is not.** The dedupe is name-string-keyed — verbatim alias vs stored `+d.name` — and
splice/clone machinery only ever mints *head* labels (`src:`/`+o:c`/`+n`, `reify:`),
disjoint from the clause-label space it matches; nothing in the quasiquote path can feed
or be hit by the dedupe. Session semantics are stated correctly: the qualified alias
unlinks the declared clause, the name dies, `learn_where`/`learn_body` is the
name-preserving route. The defect is at the next stage — the unlink is an unrecorded
program-side mutation and (d)'s replay spec has no step that re-enacts it — MAJOR 1.

**`learn_method` corollary (:1803-1809) — verified correct.** A qualified `run`-role
record is only ever produced by `code_named(L)` with a qualified `L` (:1785-1788,
:1859-1860); qualified `L` can never match a head label at emission (labels can't carry
the qualified shape), so the slot is always unwritten and only genesis's rebind can fill
it; the only *durable* head-valued symbols are `learn_method`-installed (declared `def`s
are entry-op-valued — head-less `run` refusal; `graft_method` symbols die with the
incarnation). `code_named("demo:Forager.f")` at a declared `def` is indeed a guaranteed
patch-drop. The corollary is right and correctly scoped.

## 3. New findings (v1.11 deltas only)

### 1. MAJOR — §6.1 :1173-1180 vs §6.6 (d) :2068-2081/:2122-2127 — the qualified-alias dedupe's *unlink* has no replay arm; the declared clause resurrects at resume

v1.11 states the session consequence — `learn_plan("Feed", "demo:Forager.travel", …)`
"unlinks the declared clause and installs `patch:<n>` — the qualified name then resolves
to nothing" (:1176-1178) — but the unlink is a *program-side* mutation: the goal's
`d.clause` rank lives in the folded image and `promote` publishes only what the agent
cell reaches (:599-601), so the unlink is **never checkpointed**. At resume the declared
clause folds back at `1..E_ops` — the very free-restoration the doc relies on at
:2119-2121 — and (d)'s replay spec is fully enumerated: reify registered head → install
copy → `+d.name` verbatim off the record → ref slots off capture endpoints. No step
re-applies a name dedupe or unlink. Post-resume the goal's rank therefore holds `{declared
clause, patch:<n>}` where the session held `{patch:<n>}`: `plan_named("demo:Forager.travel")`
resolves again (the "dead" name resurrects), plan selection sees a clause the session
removed, and — worst — *no* patch records the declared name, so `unlearn` cannot remove
the resurrected clause at all (program-original attachments record no undo, :1105;
`prune` is session-only). The "re-runs each graft" phrasing (:392, :2122) doesn't save it:
`graft_plan` has *no* dedupe semantics — same-named grafted clauses are the stated
surviving ambiguity (:1185-1186) — the dedupe is documented as `learn_plan`-level, matching
the *alias* (:1173-1176), and the alias is simultaneously declared "never a lookup key…
write-only audit metadata" (:1163-1164), which forecloses the natural fix in the same
breath. (`learn_method` likely converges only because `graft_method`'s own definition is
unlink-first — but under a bare-attach reading of (d) it diverges too: the folded declared
symbol sits earlier in the `d.vars` rank and first-match-wins shadows the learned method.
That dependency is also unstated.) An implementer following (d) builds a divergent replay.
**Fix:** state that a clause-install replay first matches the recorded alias verbatim
against clause `+d.name`s and unlinks a hit — the alias thereby becomes a replay key (say
so: "the alias is replay-keyed, never a session lookup key") — or record the unlinked
clause's name on the patch as a first-class field; and state that a symbol-install replay
re-applies `graft_method`'s unlink-first against the recorded qualified name.

### 2. MINOR — §6.6 P16 paragraph :1891-1897 — pre-R11-B store-role phrasing survives the reroute

Two stale remnants in one paragraph: "the adapter constructs and keeps the **publish**
`Store`" (:1891-1892 — the only unrenamed instance; every other site now says lineage
`Store`), and "at `resume` supplies it as the rebuild's backing store **or verifies the
rebuilt store's `1..E_ops` band against it**" (:1896-1897) — where "it" reads as the
adapter's seeded store. Post-R11-B the seeded store is explicitly "not the comparand"
(:1531-1535): the verify arm reads the *runtime's own* base-store band
(`arena.base()->store()`, :1528-1530) against the decoded records, and `publishValue` runs
against the received-band store (:1579-1584). Under the verify arm the kept store is not
the publish store at all, so "the publish `Store`" misleads at the paragraph that defines
the seam. **Fix:** rename to "the lineage `Store`" and re-point the verify clause:
"or verifies the received band in the runtime's own rebuilt store —
`arena.base()->store()`'s `1..E_ops` in canonical post-remap form" (the §10.1/:2652 and
P16-row phrasings, which got it right).

### 3. MINOR — §6.1 :1164-1167 — the alias→clause join is unstated, and it is the only name-keyed rediscovery path

"a learned clause is re-found by `clauses_of` + the `patch:` prefix or
`plan_named("patch:<n>")` with the ordinal read off the `d.patch` rank" — but neither arm
discriminates *by the alias the user supplied*. With two learned clauses on one goal,
`clauses_of` yields two `patch:<n>` labels and the alias is nowhere on the clause — it
exists only on the patch record; `plan_named("patch:<n>")` needs the ordinal, which the
caller can only get by walking `d.patch` records and matching the **alias field** to the
record's `+d.name`/ordinal. That join — `cells"$a/+d.patch"` → per record read alias and
label — is the sole mechanism that maps the name the user knows to the cell `unlearn`
needs, and the doc doesn't say it. This also qualifies "never a lookup key": the alias is
not a *resolver* key, but it is the rediscovery join key — worth saying plainly, since
MAJOR 1's fix makes it replay-keyed too. **Fix:** one clause — "the ordinal for a given
alias is read off the `d.patch` records' alias field, which is the only name→clause join;
`clauses_of` + `label_of` enumerates learned clauses without the alias".

### 4. MINOR — §6.6 :1731-1745 + :1674-1676 — `unlearn`'s clause arm cannot distinguish a `d.patch` record cell from a clause

v1.11 makes the shared label explicit and intentional: "Patch and installed clause share
the `patch:<n>` `+d.name` intentionally — the patch's label names the clause it installed"
(:1674-1676). The disjointness argument covers `plan_named`'s space but not `unlearn`'s
arg: the clause arm's reverse read takes *any* cell's `+d.name`, and a `d.patch` record
carries that exact shape. `unlearn(patch-cell)` then resolves the patch's own minted
label — which names a *different* patch's target (the install patch `patch:7`'s label
equals its own recorded target only coincidentally; a `learn_where` patch `patch:12`
records target `patch:7`, so `unlearn` on the patch:12 cell seeks target `patch:12` —
popping an unrelated patch on clause-12 if one exists, else silently no-op'ing where the
caller expected undo). A `cells"$a/+d.patch"`-enumerating reader can hand the verb exactly
this wrong shape with no refusal. **Fix:** the clause arm requires a `-d.clause` backlink
(patch records sit on `d.patch`, distinct dims — one shape check refuses the confusion)
or states patches-arg is out-of-contract and the result of feeding one is a fault, not a
silent no-op/mispatch.

### Nits

- :2068-2069 — "(replayed grafts mint no patch cells, so no serial is consumed)" —
  replayed `reify`s do consume counter serials (each mints `reify:<src>:<serial>`); true
  only of the patch-ordinal draw. "…so no patch ordinal is minted" is the accurate shape.
- The `d.vars` counter's cell *shape* is never stated — anonymous scalar vs named symbol.
  If it were a named cell on the agent's `d.vars` rank it would join the same-name space
  `learn_method`/`symbol_named` walks and a user member could collide; say "an
  adapter-minted anonymous scalar on the agent's `d.vars`" (or reserve its name).
- :1162-1163 — "alias `+d.vars`-keyed beside `ordinal` and `priority`" — `ordinal` and
  `priority`'s keying dims are never given, and the phrasing can be read as all three on
  `+d.vars`, which single-occupancy forbids.
- :1185-1188 — the grafted same-name ambiguity's *`unlearn`* face is undrawn:
  `unlearn(grafted clause named "demo:Forager.travel")` reverse-reads the name and pops
  the latest patch recording it — undoing a patch on the *declared* clause, not the arg.
  Same acknowledged ambiguity class; one clause noting `unlearn` inherits it.
- `plan_named(qualified: Str)` (:1089) — the parameter name `qualified` now also covers
  `patch:<n>` spellings (works only through the reserved-prefix fallback to verbatim);
  cosmetic.
- :1176-1179 — the intention-side consequence of the killed qualified name is undrawn:
  `tried` records plans by qualified name (:1153-1154), so an unlinked declared plan
  leaves dead names in intention bookkeeping — worth half a clause that this is the same
  exposure `prune` already carries.

## 4. Staging-discipline summary

v1.11 lands all three of my r11 minors in the ruled shape, and the serial design itself is
sound: a single durable monotone counter shared by `reify:`/`patch:`/validate gives global
uniqueness with only sparse-ordinal fallout, which every consumer tolerates. The
learn→name→unlearn cycle closes mechanically, and the `learn_method` qualified-success
corollary is exactly right. The one real hole is a staging-faithfulness gap in the newest
consequence: the qualified-verbatim dedupe is specified only for its session arm — its
unlink is an unrecorded, uncheckpointed program-side mutation that replay cannot
reconstruct, so the EDL stops reconstructing session state precisely where v1.11 made the
dedupe load-bearing.

VERDICT: NOT CONVERGED — 0 blocking, 1 major (dedupe unlink has no replay arm → declared
clause resurrects post-resume), 3 minors (stale "publish `Store`"/verify-vs-seeded-store
phrasing at :1891-1897; alias→ordinal join unstated; `unlearn` can't refuse a `d.patch`
record arg), 6 nits.
