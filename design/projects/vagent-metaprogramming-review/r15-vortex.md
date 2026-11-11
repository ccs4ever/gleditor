# Round 15 critique (vortex role): vagent-metaprogramming.md v1.14

Lane: VortexCore/VM execution semantics, store/spool positions, handle
relocation, promote, genesis pass order. Convergence check on the v1.14 deltas:
the `closed` program-side `+d.name` carve-out, the restated frontier, the
(d)-time `*handleTarget` read, the agent-legs mechanism, and the "capture
edge/link" vocabulary sweep — plus a fresh defect hunt over the whole `git
diff` (15 hunks, ~140 changed lines).

## Verification of the r14 dispositions

### `closed`'s program-side `+d.name` carve-out — verified, and the (b)/(c) split is mechanically distinguishable

The orchestrator's operative question was whether a recipe-record `+d.name`
arm mechanically distinguishes a dim-name endpoint from a cell-name endpoint.
**It does, by record class — never by name content:**

- Arm (b) records hang on the head's `+d.refs` rank and pair `+d.ref → slot`;
  their `+d.name` resolves through the head-label/qualified-name walk and
  rebinds `int64(entryOp|head)` (:1042-1051, :1938-1939).
- Arm (c) records hang on the head's `+d.recipe` rank and pair
  `+d.refFor → slot` (:1143-1144, :2227); their `+d.name` resolves through
  `ensureDimension`'s name arm (`arena_manifold.cpp:78-105` — `dimensionNamed`
  over base/cache before mint).

Different rank, different correlation dim, different resolution machinery —
no textual collision possible. A `+d.name` sitting on a `+d.refs` record can
only be arm (b); on a `+d.recipe` ref-capture record only arm (c). The
discriminator at :1046-1051 reads `+d.name` presence on `+d.refs` records
specifically, so a ref-capture `+d.name` cannot be mistaken for a name-record
(the REF-record/name-record marker decision never sees it). One-record-one-edge
also means a ref-capture record carrying `+d.name` cannot also carry
`+d.values`, so `+d.name`-bearing ⇒ dim referent is unambiguous within
`+d.recipe`.

The carve-out sites all check out:

- **:1171-1173** — "a `+d.name`-bearing program-side record's endpoint being
  the named dim rather than a cell and counting as name-resolved" — applied
  where §6.1's enumeration needed it.
- **:1859-1865** — arm (c) stated with the right internals: the slot's `+d.refs`
  record stays a REF-record (a `+d.name` there "would make it a name-record and
  skip the marker ceremony" — correct: the REF-record keeps the
  `d.mesh.relocation` marker → `OpHandle(dimRef)` → promote's handle-target
  walk drags the dim into the durable set, `arena_manifold.cpp:1759-1764`,
  while `+d.name` supplies the re-resolution channel). The refused class is
  correctly stated for both carriers: "`cap$N` link or an unnamed slot landing
  on a program-side cell ... carries none of the three" — and cap$N slots
  genuinely cannot take arm (b) (no `+d.refs` record exists for a clone
  operand: its `+d.clone→master` edge folds verbatim, no genesis rewrite, no
  ref-record needed).
- **:1887-1890** — the operative check "each endpoint is state-side or
  name-resolved under (b) or (c) — a `+d.refs` name-record or the paired
  record's `+d.name`" is correct as a *presence* test, which is the only thing
  checkable at accept time (name-record resolvability is a genesis concern —
  the (e) ordering passage at :2255-2259 requires post-installation symbols,
  and `ensureDimension` is total anyway, so (c) can never dangle).

I attacked the convergence once more from the (e) side: for a session-minted
(ephemeral) dim, `*handleTarget` gives its promoted position `P_new` (>E_ops,
appended); `ensureDimension(name)` finds the same cell because the dim's
`d.dims` rank membership survives the fold — promote's BFS queues the handle
target (`:1762-1763`), walks its `dimensionsOf`, and discovers the `-d.dims`
rank predecessor into `seen` (`:1782-1783`), so `home +d.dims→dim` lands in the
emitted subgraph and `Manifold::dimensionNamed`'s `dimensions()` scan finds it
(`manifold.cpp:782-821`). For an image dim ≤E_ops both arms trivially agree.
The name arm is thus redundant-but-harmless where `*handleTarget` already
answers, and the two channels cannot disagree.

### `closed` frontier restated (:1880-1884) — verified

"`+d.refs` records, the emission's wing-dim `cap$N` capture-slot links, the
`+d.recipe` ref-capture records whose endpoints are read off their paired
slots' content (§6.1), and the head's own structural dims" — matches the actual
emission inventory. The two named §11-door evasions (non-op-graph dims under
U-M7's no-`dimensionsOf` bound; int64-content program refs invisible to the
link walk) are honestly scoped. Emission-time `+d.clone` edges exist only on
cap$N capture slots — a `Clone` op's output slot is a scalar wing cell whose
`+d.clone` edge is minted at run (`vortex_vm.cpp:807-836`), not in the emitted
graph — so "the emission's wing-dim `cap$N` capture-slot links" is a complete
enumeration of the clone-edge class.

### (d)-time `*handleTarget` read (:2241-2245) — verified

"internal and state-side endpoints ride the slot's `int64` ref position
in-session and its `*handleTarget` at (d) — the donor's marked ref cells still
`OpHandle`-kind, (e) rehydrating last" — exactly right. At (d)-replay the
donor's marked cells hold promoted `OpHandle` targets (`real.at(target)`,
`arena_manifold.cpp:1947-1951`); `asInt64` would fail on them; the r14 phrasing
slip is fixed and the `+d.name`/program-side arm is correctly named beside it.
The "external arm only" honesty qualifier on the `int64(endpoint)` ≡
`*handleTarget` equality survives at :2248-2249.

### Agent-legs mechanism (:2134-2143) — verified, both halves

- The `cap$N` surviving case is mechanically exact: the capture slot is a
  clone on the operand's wing — `wingOf` decodes `asInt64`-fail →
  `masterOf(slot)` (`vortex_core.cpp:356-365`), `masterOf` = `cloneMaster` =
  `rankEnd` NEG-ward on `d.clone` (`arena_manifold.cpp:1008-1012`), and `Clone`
  appends `source -d.clone→ clone` so the master sits at the NEG end
  (`vortex_vm.cpp:807-836`, `vortex_core.cpp:334-341`). A fragment-internal
  slot's `+d.clone` chain to an agent-`d.vars` binding cell is a genuine
  cross-root edge — program graph to agent root.
- "a ref-capture record contributing none since its referent rides slot
  *content* the link walk cannot follow" — the R13-A content-carried rule,
  correctly negative.
- The conclusion ("no agent-rooted second walk is owed — its only consumer is
  `closed`'s frontier enumeration") follows: every such leg classifies `>E_ops`
  anyway.

### Vocabulary sweep — verified

:137 (glossary `closed` row — now names cap$N clone link OR slot-carried int64
under "however carried", plus the paired-ref-capture `+d.name` dim arm), :136
(glossary `patch` row — "no session resolver consults it" mirrored), :1176-1178
("capture slots carry the same definition-site positions"), :1853-1854,
:2138-2140, :3604-3606 (1.13 changelog entry corrected in place per the
disposition). Remaining "capture link/slot" occurrences (:818-823, :1690-1692,
:1853, :1881, :3281, :3605, :3619) are all the *real* cap$N clone-edge class or
historical changelog text — none resurrects the dropped record-dim capture
edge.

### Other adopted minors — verified

- **Recipe-tuple `+d.values` conditional** (:1137-1142): "each endpoint field
  pointing `+d.values` at the actual endpoint cell under the same
  one-record-one-edge rule ... a claimed `-d.values` taking no edge, the
  endpoint then riding the field's `int64` content for the old→new map to
  translate either way" — the shared-endpoint collision ("an op cell is `to`
  of one tuple and `from` of the next") is exactly the M14 single-occupancy
  class, and the content-channel fallback is the same mechanism as the
  ref-capture arm. Consistent.
- **`documentId_` ctor-or-load** (:633-636, :1562-1565, :2787-2789): cites all
  exact — `store.cpp:135` (`documentId_ = documentId;` in the two-arg ctor,
  decl `store.hpp:132-133`), `store.cpp:4450` (`documentId_ = tables.documentId;`
  on the store-tables load), `store.hpp:1435` (the field).
- **(v)'s `Store()` cite** (:2861): `store.cpp:123` is the `Store::Store()`
  signature line — the no-id ctor — correct fix from `:124`.
- **Displaced-vs-appended bound** (:1234-1237): "auditable for a *declared*
  victim — its `+d.name` folds back in the `1..E_ops` band ... a session-grafted
  victim mints no `d.patch` record of its own" — correct bound; a grafted
  clause does hold `+d.name` verbatim (:1223) but folds nowhere, so nothing
  proves the alias named anything. Sound.
- **Grafted-twin restatement** (:1240-1244): "displaced nothing durable
  (grafts never fold back), so the replay re-matches the folded rank, appending
  where no same-named clause survives and displacing where one does" — the
  replay-displace caveat kept; deterministic both ways as before.

## New findings (v1.14 deltas only)

### 1. MINOR — arm (c)'s "a *program-side* dim referent" over-narrows: a session-minted dim is ephemeral, and under the arms' letter carries none of the three

**Location:** §6.6, :1859-1861 — "or (c) a program-side dim referent
name-recorded on its paired *ref-capture* record's `+d.name`".

**Problem.** A `dimConstant` naming a *session-minted* dim holds
`int64(E_dim)` — an ephemeral endpoint. Under the letter of the three arms it
is refused: arm (a) needs "link-reachable *from the agent cell*" (:1855) and a
fresh dim hangs on `home`'s `+d.dims` rank, unreachable from the agent root
(:1700-1705); arm (b) needs a `+d.refs` name-record the dim slot lacks; arm (c)
needs a "*program-side* dim referent" — but the sideness test classifies any
ephemeral ref as state-side (:2144-2147), so an ephemeral dim is not
program-side under the test the arm invokes. Yet the operative check at
:1888-1889 admits the case outright — "or the paired record's `+d.name`" makes
no positional test — and rightly so, since `ensureDimension`'s name arm
resolves an ephemeral dim identically at genesis. The arm's definitional
letter is narrower than the check that implements it.

**Evidence:** `findOrMintDimension`/`dimConstant` mints
`literal(int64(dimRef))` with an arena-local (ephemeral) ref,
`compiler.cpp:769-772`; `ensureDimension` mints ephemeral cells,
`arena_manifold.cpp:87-89`.

**Fix.** Drop the position qualifier — "(c) a *dim* referent name-recorded on
its paired *ref-capture* record's `+d.name`" — or add "program-side or
session-minted: the `+d.name` arm resolves either". A state-side dim endpoint
is admitted under (a) regardless, so the qualifier buys nothing and the
mismatch invites an implementer to add a `≤E_ops` test the check does not make.

### 2. MINOR — the operative check's "the paired record" never names the pairing machinery

**Location:** :1888-1889 — "a `+d.refs` name-record or the paired record's
`+d.name`".

**Problem.** "The paired record" is the `+d.recipe`-rank ref-capture record
paired by `+d.refFor → the same slot cell` — stated at :1143-1144 and
:2227-2228, but the re-verification sentence is where the (c) arm is exercised,
and a `+d.refs` record *also* pairs the slot (by `+d.ref`). A reader local to
§6.6 can legitimately scan "those records" for any slot-correlated `+d.name`
and ask whether a name-record's own `+d.name` satisfies (c) (it does not — (b)
already claims it, and a name-record is not ref-capture-paired). The (b)/(c)
mechanical distinction I verified is sound, but this sentence is where it must
be self-evident.

**Fix.** "— a `+d.refs` name-record or the `+d.refFor`-paired `+d.recipe`
ref-capture record's `+d.name` (§6.1)".

### 3. MINOR — the (d) hand-off lists the conditional `+d.values` among "the referent class" suppliers, but the internal/external discriminator is map membership, not record fields

**Location:** :2214-2217 — "the record supplying the pairing and the referent
class (`+d.refFor`, the conditional `+d.values`, `+d.name`), never the endpoint
value itself".

**Problem.** As a field inventory the parenthetical is complete, but as a
*class* read it over-credits the record: `+d.values` presence ⇒ internal is
reliable, yet its absence is ambiguous — an internal endpoint whose `-d.values`
was claimed carries neither edge (§6.1's occupancy rule, :1160-1162), and
field-only classification misreads it as external. §6.1 itself assigns the
discriminator correctly: "map membership *is* the internal/external
discriminator" (:1159), and :2246-2249 restates it ("an *internal* referent is
rebound through the old→new reify map"). The sentence does not contradict the
mechanism — it just invites an implementer to derive class from fields the
fields cannot always encode.

**Fix.** "the record supplying the pairing and the program-side mark
(`+d.refFor`, `+d.name`) — the conditional `+d.values` only ever annotating an
internal endpoint, the internal/external split itself read off map membership
(§6.1) — never the endpoint value itself".

## Nits (non-blocking)

- :1172 and :1859-1860 — "the endpoint being the named dim rather than a cell":
  dims are cells (`ensureDimension` → `makeCell`, `arena_manifold.cpp:87-89`).
  The intended contrast is "resolved by name rather than carried as a
  graph-cell position"; as written it asserts a false type split. Consider
  "the endpoint re-resolving to the named dim rather than being carried as a
  cell position".
- :1859 — "name-recorded" as a verb sits one hyphen from "name-record", the
  defined noun for `+d.refs` records; the same sentence's REF-record
  parenthetical rescues it, but "`+d.name`-recorded" or "name-marked" would
  not tempt the conflation.

## Summary for the orchestrator

The carve-out is mechanically airtight: the (b)/(c) distinction is carried by
record class — `+d.refs`/`+d.ref` records resolve names through the head-label
walk, `+d.recipe`/`+d.refFor` records resolve them through `ensureDimension` —
so a dim-name endpoint and a cell-name endpoint are distinguished structurally,
not by name content, and both resolution paths are verified against code
(`arena_manifold.cpp:78-105`, `:1759-1764`, `manifold.cpp:782-821`;
`vortex_core.cpp:356-365`, `:334-345`; `vortex_vm.cpp:807-836`). The frontier,
the (d)-time `*handleTarget` read, the agent-legs restatement, and the
vocabulary sweep all verify; every code cite re-checked exact
(`store.cpp:123`,`:135`,`:4450`; `store.hpp:132-133`,`:1435`). Three MINORs,
all wording precision at the (c) boundary — a position qualifier that is
stricter than the operative check, an unnamed pairing, and a field-list that
over-credits record fields against the map discriminator — plus two nits.

VERDICT: CONVERGED
