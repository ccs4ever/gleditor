# Round 10 critique — `design/vagent-metaprogramming.md` v1.9 — Metaprogramming Expert lane

Scope: verify the r9 major (genesis-(a) landmark conflation), my three r9 minors
(`code_named(L)` unresolved-at-emission + headless-`run`; "current contents" → capture
endpoints; `patch:<n>` `<n>` source/user collision) and the carried nits; re-check the staging
story after the `run` narrowing; assess `label_of`'s new `cells"…"` acquisition path; hunt new
defects in the v1.9 deltas only (diff reviewed in full, `git diff 9fc8c9c7..11fec218`).

## 1. The r9 major — FIXED, and fixed in the ruled shape

Genesis (a) at :1801–1836 implements R9-A exactly:

- **Two landmarks, separated.** "the band bound is `E_ops`, the image prefix length *in ops* —
  while a cell's persistent ref is its *birth-op* position (`manifold.hpp:295`,
  `store.hpp:677-681`), and the image's last op is never a birth (`promote` emits links last,
  `arena_manifold.cpp:2010-2017`; `exportSymbol` likewise closes with the `+d.values` and rank
  links, `vortex_core.cpp:119-127`), so no walk maximum can ever equal `E_ops` and none is
  asserted." The unsatisfiable quantification is gone outright, with the *reason* stated.
- **Per-cell classification, never refusal** (:1820–1828): "each cell the `program()`
  structural walk reaches — the walk rooted at the adapter-recorded program root resolved on
  the received fold, (c) re-binding it for in-arena use — tests its own persistent ref against
  `E_ops`: `≤ E_ops` classifies program-side, `> E_ops` state-side — so cells the walk
  legitimately reaches beyond the band through `+d.code`, `d.vars` or `d.belief` (registered
  heads, member state) are classified state-side, never refused". This is precisely the
  boundary-not-violation semantics my r9 finding asked for; the `d.vars`/`d.belief`/`+d.code`
  inclusions in the walk vocabulary stop being liabilities and become the classification's
  inputs.
- **Merged-lineage detection moved to a mandatory ops-segment content comparand**, canonical
  post-remap form (op kind + non-scroll fields — `publication.cpp:1048-1086`,`:1030+`), and
  the coverage split is stated as the ruling required: the comparand detects
  permutation/scramble inside the band (`adoptOpRecords`' topo-resort, `store.cpp:3836-3940`)
  and the within-band non-image occupant; the per-cell test classifies ingress above it
  ("an image twin minted above `E_ops` reads as state"). The `mergeValues` fork is stated
  correctly this time — `(contentHash, authorKey, parent)` order (`mesh_value.cpp:519-523`),
  the no-`job.base` fresh-destination arm (`mesh_host.cpp:509-510`) failing outright, the
  `job.base` arm (`:507`) appending above the base's prefix legitimately.
- **Companions landed.** (b) now keys registration on "the `1..E_ops` band the comparand of
  (a) just verified" and runs at the spawn-time bind too, with the honest consumer statement
  (`opcodeMap_.contains(masterOf(cell))` is what P15 tests; `getOpcodeKind`'s `asInt64`
  fallback decodes without membership). R9-B's base-injection is real code-shape:
  `WorkerContext(const Manifold*, const SpanReader*)` (`worker_context.hpp:52-55`) is
  resume's own injection (`mesh_actor.cpp:270-271`), `initGenesis` adopts `*base->home()`
  only when a base exists (`vortex_core.cpp:30-36`) inside the ctor (`:14`) with no re-genesis
  door, and `rebuildManifold` returns `Manifold` by value (`store.hpp:450-451`) so pinning is
  honestly owed. R9-C: (d) now says the copy's ref slots "derive from the *capture-record
  endpoints* at replay — its rehydration source being the record rather than the donor cell's
  live value (§6.1's recorded-template rule)" (:1855–1857), and (e) prices the stale-alias
  alternative (:1873–1883) — the mechanism is now the record's `+d.values`-named referent fold
  cell, not "current contents".

The fresh-store detection arm is also better than ruled: `ArenaManifold::store()` public
(`arena_manifold.hpp:269-271`, falling through to `base_->store()` — verified) plus
`Manifold::store()` `const noexcept` returning non-const `Store*` (`manifold.hpp:486` —
verified) means only *sanctioned supply/enforcement* is owed, correctly scoped.

## 2. My r9 minors — all landed

- **`code_named(L)` unresolved-at-emission + headless-`run` — CLOSED** (:1623–1636). The
  static-slot reading is adopted nearly verbatim: "`code_named(L)` … a name, not a call: the
  emitter resolves `L` through `code_named`'s two-space lookup at emission and writes
  `int64(head)` into the slot on a hit, while an `L` it cannot resolve — a qualified spelling
  like `demo:Forager.f` that no head label can carry, or a `reify:`/`d.code` label the
  emission set cannot yet contain — leaves the slot head-less while still recording `L`, so
  the session `run` refuses it at the §6.2 descriptor check *until the record's genesis
  rebind fills the slot*". The qualified-`L` route is then the documented escape hatch:
  genesis resolves it through the qualified path but "must land on a head" (:1681–1686), a
  head-less `def` resolution being the headless-`run` refusal with the patch dropping like an
  unresolved name — consistent on both sides of the boundary.
- **"current contents" → capture endpoints — CLOSED** (above; :1855–1857, :1873–1883).
- **`patch:<n>` `<n>` source + collision — CLOSED** (:1114–1117): `<n>` is pinned to "the
  patch's `d.patch` ordinal" and "`patch:` is refused as a user-supplied `name` at
  `graft_*`/`learn_*` emission (the same reservation §6.7 gives it at module declaration)" —
  durable-unique by monotonicity, and the verbatim-match collision door is shut.
- **Nits — all resolved.** `+o:`'s arm is now "the `+<off-line>:<off-col>` pair §5.1's
  diagnostics use inside its enclosing body" (:1923–1924); "`patch:<n>` their largest
  producer" names the clause-label guard's motivation (:1692); the `d.code`-walk nit is moot —
  the sentence was rewritten to "two-space lookup" (:1625); the `+d.refs` diagram xref is
  relabelled `arena_manifold.cpp:1347-1377` / M14 (:1010–1011); `plan_named`'s row no longer
  claims declared handlers (:1058).

## 3. Staging story after the `run` narrowing — coherent

The quasiquote/splice/learn cycle holds together under verbatim `code_named` matching +
`patch:<n>` clause labels + unresolved-at-emission:

- **Routing is by shape, disjointness airtight.** The `+d.name` record's first-`:` test fires
  the qualified path only when the qualifier resolves as a module (:1668–1673, :1942–1950);
  `src:`/`reify:`/`patch:` are reserved at module declaration so no label reroutes; user
  `#label`s cannot spell `:`. A qualified `run`-role record (`code_named(L)`, the only
  producer) resolves `symbol_named` → `+d.values` → the op-or-head hop → head-required,
  `int64(head)`; a verbatim label resolves the program-image label→head walk restricted to
  head layout (`+d.entry` + descriptor `+d.values`) so clause `+d.name` labels can't match;
  learned (`reify:`) labels fall back to `code_named`'s `d.code` walk (:1689–1697) — which is
  populated by the fold itself before (e) runs, since `d.code` rides the checkpointed state.
- **The three head-less cases now form one class.** Emission-unresolved `L` (slot never
  written) → session `run` refuses at the descriptor check; genesis-resolved-but-headless
  (`def` symbol) → record unresolvable, patch drops; `Halt`-tail/`def`-symbol session `run` →
  §6.2's descriptor refusal. One refusal class, three entry points, no silent binding.
- **Replay ordering is still right.** (e) runs post-(d), so a qualified `run`-role record
  naming a patch-installed method resolves the *replayed* symbol→head; the divergence clause
  (session keeps emission-time binding, genesis follows post-resume re-points) is consistent
  with the static-slot reading now stated.

One acknowledged asymmetry, now honestly specified rather than a hole: `run(code_named
"demo:Forager.f")` is dead in-session (qualified `L` can't be a head label) yet live after
genesis — the designed escape hatch, not a bug.

## 4. `label_of`'s `cells"…"` acquisition — sound shape, one wrong anchor (finding 1)

The *shape* is the right reflective read: `label_of` takes `Cell`, the head-producing reads
(`code_named`/`test_of`/`body_of`) yield `Code<Cell>`, M3 bars `Code`→`Cell` coercion, so
`cells"…"` navigation — which yields `Cell`s — is genuinely the only channel. The
installed-head arm `cells"$clause/+d.spin"` is correct (clause `Cell` from `plan_named`/
`clauses_of`, `/+d.spin` to the reify-labelled copy). But the registered-head arm anchors the
wrong root — finding 1.

## 5. New findings (v1.9 deltas only)

### 1. MINOR — §6.1 `label_of` row (:1060), inherited at §6.7 :1961–1964 — `cells"##/+d.code"` anchors at *home*, but `d.code` lives on the agent cell; the named path yields `[]` silently

`##` is the **arena home cell** (`vagent-agent-language.md:1257`; lowered to `core_.home()`,
`vagent/compiler.cpp:749-750`) — the program-side root carrying `+d.stdlib`/`d.dims`. `d.code`
is "the agent's registration rank" (:1346) — "the top-level ranks themselves — `d.code`,
`d.patch`, `d.synapse` on the agent cell" (:1538–1539) — and `agent_cell()` exists precisely
because "`cells"…"` anchor for state ranks" can't be `##` (:1052; §9.3 binds
`let a := agent_cell()` then walks `cells"$a/+d.synapse"`, :2142–2143). So `cells"##/+d.code"`
walks home's `+d.code` rank, which holds nothing — and per spec "a traversal on an empty rank
yields `[]`, never fails" (`vagent-agent-language.md` §8.7), the miss is *silent*: a reader
following the doc to read a `reify:` label off a registered head gets an empty list and
`label_of` never runs. This is the only stated acquisition path for registered heads, so the
R9-F3 fix names a broken path in its primary arm. Fix: "`cells"$a/+d.code"` with
`a := agent_cell()` for a registered head" — matching §9.3's idiom — keeping
`cells"$clause/+d.spin"` as-is; §6.7's "the head reached as a `Cell` through `cells"…"`
(:1962) inherits by reference and needs no separate fix. (While there: `cells"…"` yields
`[Cell]`, a rank list — worth one word acknowledging the iteration/`for` step that produces
the single `Cell` `label_of` wants.)

### 2. Micro-nits

- :1627–1628 — "leaves the slot head-less": the *observable* content of a head-less slot is
  never named — what the descriptor check reads to refuse (a `None` write? `noCell`/0?
  `+d.entry` absent?). The slot is a real emitted cell, so one phrase pinning the marker
  would keep an implementer from inventing a sentinel (house rule: no reserved values —
  presumably the slot is left unwritten/`None`, which `run`'s `+d.entry` deref already fails
  cleanly).
- :1625–1630 — emission-fixed slots imply a same-session asymmetry worth one clause: an `L`
  unresolved *at emission* stays refused even if a later `learn` in the same incarnation
  registers that very label — `code_named(L)` would then resolve, but the slot was never
  written and only genesis's rebind fills it. A `learn` + `run` in one session is the natural
  flow; stating "the slot is emission-fixed; an in-session registration does not fill it —
  the record fills at genesis" prevents the surprise being read as a bug.
- :1962 — phrasing: "the head reached as a `Cell` through `cells"…"`" is elliptical in the
  same way the row is; if finding 1's fix lands with the `[Cell]`→`Cell` note, this clears
  for free.

## 6. Staging-discipline summary

v1.9 is the cleanest staging round yet. The R9-A repair is not a patch on the broken test but
the correct two-landmark redesign — band comparand for identity, per-cell classification for
sideness — and it *improved* the architecture around it (spawn/resume asymmetry collapsed to
one injection shape; `opcodeMap_` registration scoped to the verified band at both binds; the
home claim moved onto the `holdsOwn` prune where it belongs). The `code_named(L)` static-slot
reading gives the escape hatch honest semantics on both sides of the boundary, `patch:<n>` is
now pinned and collision-proof, and the whole `run`-operand taxonomy — literal → label record,
`code_named` → verbatim record, qualified `L` → qualified path, opaque/`reflect`-born →
closedness refusal — is finally closed under both emission and genesis. The one new defect is
a wrong `cells` anchor token in `label_of`'s example path — the reflective read's *shape* is
sound and its defect is one identifier (`##` for `$a`), but as written the named path yields
the empty rank.

VERDICT: CONVERGED — 0 blocking, 0 major, 1 minor (`label_of`'s `cells"##/+d.code"` anchors at
home instead of the agent cell), 3 nits.
