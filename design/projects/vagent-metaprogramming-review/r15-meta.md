# Round 15 critique — `design/vagent-metaprogramming.md` v1.14 — Metaprogramming Expert lane

Scope: verify my three r14 items (the `closed` enumeration harmonization; the program-side
writeback the reviser flagged unapplied; the `cap$N`/`masterOf` naming in the R14-A agent-legs
restatement), judge whether the staging model is now closed end-to-end, and hunt new defects in
the v1.14 deltas only (full diff `git diff f252c2b8..406d079b` reviewed; code citations
re-verified against `apps/common/xanadu/`).

## 1. Minor verification — two fixed, one still open

**`closed` enumeration — FIXED and harmonized.** §6.6's arms (:1851-1865) now enumerate all three
admitted classes: (a) a state-side endpoint *however carried* — the `cap$N` capture link or the
`cells"…"` path whose referent rides the paired slot's `int64` content; (b) the `+d.refs`
name-record (qualified name / `run`-target label); (c) the program-side dim referent named on the
paired *ref-capture* record's `+d.name`, "the endpoint being the named dim rather than a cell and
counting as name-resolved." The refused class is correctly restated (a `cap$N` link or unnamed
slot landing on a program-side cell carries none of the three, :1862-1865). The frontier
(:1880-1884) now lists `+d.refs` records, wing-dim `cap$N` capture-slot links, and `+d.recipe`
ref-capture records read through paired slot content — the content-read I asked for — and the
accept-time re-verify (:1887-1889) names (b)/(c) by their record shapes. The glossary row (:137)
and the 1.14 changelog (:3623-3631) match. The (c) parenthetical — the slot's own `+d.refs`
record stays a REF-record because a `+d.name` there "would make it a name-record and skip the
marker ceremony" (:1861-1862) — is the right subtlety: an unmarked dim slot would fold its int64
verbatim. Verified coherent.

**`cap$N`/`masterOf` naming — FIXED and code-verified.** :2134-2143 now gives the surviving
mechanism: an agent-`d.vars` binding cell hangs on the agent cell's own `±d.vars` rank and enters
the sideness walk "only across a genuine cross-root edge — a §5.3 `cap$N` capture slot resolving
to its binding cell through `masterOf`/`d.clone`". That matches §5.3's own decode ("the emission
wraps the binding cell in a capture-slot cell ... `asInt64` fails → `masterOf(slot)`",
:822-823) and the code: `masterOf` = `cloneMaster(cell, dims_.clone)`
(`vortex_core.cpp:344-346`), the operand lambda `vortex_core.cpp:358-365`, `Clone` appending to
the source's `d.clone` rank (`vortex_vm.cpp:813-829`). I also verified the new negative claim —
"a ref-capture record contribut[es] none since its referent rides slot *content*" (:2139-2140) —
holds under R13-A: the conditional `+d.values` may-edge is scoped to *internal* referents
(:1160-1162), the external arm is explicitly "no edge at all" (:1163-1165), and `+d.name` names a
fresh name cell — so no ref-capture edge crosses the root boundary. Correct on both counts.

**Program-side writeback — NOT fixed; re-reported as Finding 2.** The v1.14 text moved the
`Int64(*handleTarget)` claim's scope to "internal and external endpoints alike" (:2225) and
corrected the (d)-time read to `OpHandle`-kind donor content (:2242-2243) — but neither (d)'s new
"derive their endpoints from the *paired donor slot's promoted content*" (:2214) nor (e)'s "per
ref-record `Int64(*handleTarget)`" (:2224-2225) states the program-side write value. The
divergence is real and now bites at *two* sites; see below.

## 2. New findings (v1.14 deltas only)

### 1. MAJOR — §6.1 :1137-1142 (echoed changelog :3635-3637): the recipe tuple `int64` arm strands a must-relocate dense id outside the relocation inventory — genesis replay mis-joins every shared endpoint

The delta gives claimed-`-d.values` endpoints a second encoding: "the endpoint then riding the
field's `int64` content for the old→new map to translate either way." That field is a plain
emitter-minted int64 ref cell, and it has **no relocation path**. P16's ceremony marks only
"every emitter-minted int64 ref cell whose `+d.refs` record carries no `+d.name`" (:379-380);
tuple endpoint fields carry no `+d.refs` records — the recipe lists them as a *distinct* record
kind beside ref-capture records (:1052-1061) — and the marker alone is undiscoverable ("no
per-cell dimension enumeration, U-M7", :1073-1074). Promote therefore folds the field verbatim
(`arena_manifold.cpp:1934-1936`: Int64 → `makeScalarCell(out.version, asInt64)` — only marked
OpHandles get `real.at(target)`, `:1947-1951). Post-resume the int64 names a dead dense id —
the exact "census integer" the same sentence refuses on the very same ground ("checkpoint
renumbers dense ids, §4.5").

The mixed encoding then cannot re-join. A shared endpoint is routine by the doc's own count — an
op cell is `to` of one tuple and `from` of the next — so its first reference folds as a
`+d.values` link (durable: resolves to the folded endpoint X@P′) while the rest fold as stale
int64s (P). At genesis (d) the replayed `reify` reads these: keyed on resolved cells the int64
lookups miss (P ≠ P′); keyed on recorded identities the endpoint splits into two copy cells. The
design's own vocabulary already contains the normalization that would make both arms join — the
1.9 changelog's restatement of R9-C: "`+d.values` naming the referent fold cell at the promoted
position, the *identical bits `*handleTarget` carries*" (:3411-3413) — i.e. marked int64
ref-content and `+d.values` links normalize to the same fold-position key. But nothing routes
tuple fields through that machinery, and they cannot simply be given `+d.refs` records: the
ref-capture pairing ("one per `+d.refs` ref-record", :1143-1144) would mint spurious capture
records for recipe-internal wiring and drag the fields into `closed`'s ref frontier. Worse, the
mixed encoding keeps an edge-into-endpoint where occupancy allows — the very shape R13-A deleted
("the referent itself is carried **as content, never as an edge into the referent**",
:1152-1153) — so it re-opens the M14 occupancy question in miniature while stranding the int64s.

In-session this is invisible (positions are live); it only corrupts at genesis replay — the one
moment the recipe exists for — and replay has no validation pass to catch it. **Fix (pick one,
one clause):** (i) state that tuple endpoint fields are *marked ref cells* inventoried on a
recipe-side refs sub-chain the ceremony walks with `+d.refs` — the uniform
content-carried/marked encoding R13-A established, which also lets the `+d.values` edge arm be
dropped entirely (a marked field never touches `-d.values`); or (ii) state the int64 names a
recipe-local identity (the endpoint's per-field record ordinal/key treated opaquely by the map),
making the recipe a position-free closed template — but then say why "never a census integer"
does not apply. What cannot stand is the literal dense-id reading.

### 2. MINOR — §6.6 (d) :2214-2217 + (e) :2223-2230 — carried: the program-side write value is still unstated, and (d)'s new wording makes the wrong reading uniform

Re-reporting the r14 minor, sharpened by verified code and by (d)'s rewrite. The chain: a
session-minted dim linked onto the home `d.dims` rank by `ensureDimension`
(`arena_manifold.cpp:91-103`) folds *without* that membership — promote's frontier skips `d.dims`
edges outright (`arena_manifold.cpp:1766-1768`), so the dim reaches the fold only through the
slot's own `handleTarget` enqueue (:1759-1764). Post-resume `dimensionNamed` consults the base's
`d.dims` rank walk (`manifold.cpp:771-779,802-815`) and the session `arenaDims_` map
(`arena_manifold.cpp:71-75,83-86`) — the orphan is on neither — so `ensureDimension`'s name arm
mints the canonical twin at a different position (:87-104). Since a `DimRef` is cell identity,
`Int64(*handleTarget)` binds the orphan while every name-resolved consumer — including the same
fragment replayed — sees the twin: a silent dim-split for `Linked`/`New` traffic.

v1.14 scoped the `*handleTarget` equality to "internal and external endpoints alike" (:2225) —
acknowledging the exclusion — but still never says what a program-side slot is written with, on
either of the two paths that now matter: (e)'s sweep still reads "per ref-record
`Int64(*handleTarget)`" over registered heads *and* install sets (:2223-2225), and (d)'s new
copy-inheritance rule says slots "derive their endpoints from the *paired donor slot's promoted
content*" (:2214) — which for a program-side donor is likewise the orphan — while insisting the
record supplies "the pairing and the referent class ... never the endpoint value itself"
(:2215-2217). That last clause is literally false on the program-side arm: there the record's
`+d.name` *is* the endpoint source — the name re-resolves through `ensureDimension` and the slot
must take the resolved twin's position, not the donor's promoted content. Note the trap is not
hypothetical: a session-minted dim's slot content classifies `>E_ops`/state-side under the
sideness test, so `closed` can admit it under arm (a) positionally even while its record marks it
program-side — the write arm must therefore be keyed on the record's `+d.name`, not on sideness.
**Fix:** state the write per arm — internal/external: `Int64(*handleTarget)` at (e), donor
promoted content at (d); program-side: `Int64(`the name-arm-resolved dim's position`)` on both,
optionally asserting equality when the promoted dim is name-findable (the artifact-era case where
the two agree).

### Nits

- :1759-1761 — the lead "the alias present only where the verb takes a `name` argument" remains
  literally false before the parenthesis corrects it (`learn_method` takes `name`, carries no
  alias); "where the verb takes a *user-alias* name" is the accurate lead (carried).
- :1160-1162 / :2216 — the conditional `+d.values` may-edge still has no load-bearing consumer
  (replay reads slot content; `closed` reads slot content; (d) lists it only as class
  information). It is defensible as recipe self-description — say so once, or drop the arm.
- :3635-3637 — the 1.14 changelog repeats the tuple-int64 rule unqualified; whatever encoding
  Finding 1 lands on should be echoed there for future diff-readers.

## 3. Staging-discipline assessment

The pipeline is closed *in shape*: quasiquote → `Code<T>` (head + descriptor + `+d.refs` +
`+d.recipe`) → `splice`/`static`/`run`/`reify`/`graft_*`/`learn_*` → `d.patch` + `d.code`
registration → P16 checkpoint ceremony → genesis (a)–(e). Every referent class `closed` admits
now has a named durable path — internal via the old→new map, state-side via folding links or
marked int64 → `*handleTarget` → (e) writeback, call/run via name-records, dims via `+d.name` →
`ensureDimension` — and every refused class is named (frame-bound bindings by class, session
handles by type, unnamed program-side links by arm, smuggled non-op-graph edges as a stated
§11-door limitation). The two residuals are both write-path details: the recipe's *own* tuple
endpoints escape the relocation inventory entirely (Finding 1 — the admitted classes are durable,
but the replay machinery that restores them has an unmarked-int64 hole in its template), and the
program-side arm still lacks its write value (Finding 2). Both are one-to-two-clause fixes inside
the existing vocabulary.

VERDICT: NOT CONVERGED — 0 blocking, 1 major (recipe tuple `int64` arm: unmarked dense-id
endpoints fold stale, and the mixed link/`int64` encoding cannot re-join shared endpoints at
genesis replay), 1 minor (program-side (d)/(e) write value still unstated; orphan-vs-twin
divergence verified), 3 nits.
