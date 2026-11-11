# R7 — System Architect critique (v1.6)

Scope: layering, ownership, adapter-vs-core boundaries, Makefile/dependency hygiene; verdicts on my
three r6 findings (extent-layout major, extent carrier, reflect-born arm) plus new defects in the
v1.6 deltas only (`:1313-1329` extent carrier/empty-set arms, `:1528-1540` publish ordering,
`:1574-1605` image-first genesis, `:2079-2084` adapter inventory, `:2103-2106` P16 row, and the
attendant M8/§6.2/glossary edits). Re-verified against the tree (`mesh_actor.cpp`, `mesh_value.cpp`,
`arena_manifold.{hpp,cpp}`, `manifold.hpp`, `worker_context.hpp`, `vagent-agent-language.md`).

v1.6 implements the r6 disposition faithfully: the extent's carrier is named, the name-record
writeback splits by slot convention exactly as R6-B ruled, the reflect-born arm is dropped, and the
minor batch is applied. The open question is whether the image-first re-materialization actually
closes my r6 major. It does not — it states the right invariant and the right preservation
mechanism, then assigns the construction to a step that cannot perform it, while the load-time
commitment that is its real locus remains unstated and is contradicted elsewhere in the same file.

## Verdicts on my r6 findings

### 1. MAJOR (the `≤ extent` discriminator presumes a persistent layout nothing sequences) — PERSISTS, restated: the invariant is now asserted, but at a locus that cannot construct it

What v1.6 gets right: the mechanism is correct and correctly cited — `birthOp == CellRef`
(`manifold.hpp:295` region — `birthOp{0}` is "the MakeCell index; == this CellRef"), so folding a
spool preserves refs verbatim; the rule "program refs `0..E` below promoted state" is stated as a
ref-space ordering, not a timestamp (`:1314-1325`); and the shadow arm (`:1330-1333`) mechanically
checks out — a shadow copies the base cell and keeps its `birthOp` ("an overlaid cell is the *same*
cell", `arena_manifold.cpp:1488-1490`), so a shadow over a persistent pre-extent base does classify
program-side.

What it still does not do is name the construction that produces `[image 0..E][state >E]` in **one**
persistent numbering — and the step list it writes the construction into cannot hold it:

- The persistent manifold an incarnation runs over is `actor->checkpoint->manifold`, folded by
  `store->rebuildManifold(version)` inside `receiveValue`/`restorePublication`
  (`mesh_value.cpp:350-351`) and installed as the arena's `const Manifold* base_` at
  `WorkerContext(actor->checkpoint->manifold.get())` (`mesh_actor.cpp:270-271`). `WorkerContext` is
  non-movable and `base_` is ctor-fixed (`worker_context.hpp:51-63`,
  `arena_manifold.hpp:259,752`) — by the time any genesis step runs, the persistent numbering is
  already complete, immutable, and single. There is no second base, no base setter, and no
  fold-at-offset remap to join a second numbering into it.
- Step (a) says genesis will "re-materialize the program image **into the persistent manifold**
  first and ref-preserved … *before* the checkpointed state joins the numbering" (`:1574-1580`),
  and step (b) says "arena overlay — the checkpointed state cells join the persistent numbering
  above the extent" (`:1582-1584`). Both describe a two-phase persistent-space construction that
  the real system delivers monolithically, upstream, inside `receiveValue`. If an implementer reads
  (a) literally and replays the artifact spool into the received manifold (it is owned mutably via
  `unique_ptr<Manifold>`), the image appends **above** the state — the exact inversion r6 named,
  now self-inflicted by the fix. If they replay into the arena or via `importValue`, the cells are
  `ephemeralBit|dense` (`mesh_value.cpp:481` mints arena cells; `refOf`,
  `arena_manifold.hpp:468-470`) — the doc's own "empty program-side set" case.
- The only reading consistent with the code *and* the doc's own "the ops-spool indices *are* the
  persistent `CellRef`s" is that the artifact's ops are already inside the agent's checkpointed
  store spool — the store anchored on the artifact at load, the `job.base` pattern
  (`mesh_jobs.cpp:303-318`: `store = base->store.get()`, `promote` appends state ops above it).
  That is a **load-time store-format commitment genesis inherits, not a genesis step it performs**
  — and it is never stated. The sentence equivocates at the load-bearing point: "replay the
  program artifact's own spool: the image's ops are the spool's low prefix" (`:1576-1577`) — the
  artifact's own spool contains nothing but image ("low prefix" of what?); only the agent's
  checkpointed spool has a low prefix and a state suffix, and nothing says the artifact is in it.
- It contradicts the file's own program-cell descriptions: M8 still says the program's module
  symbol table, `d.clause` ranks, `+d.grab`/`+d.spin` attachments and `d.vars` slots "are all
  arena cells" (`:284`) — ephemeral cells, which can never satisfy `ref ≤ E`. And the parent spec
  states the load model flatly: "Each agent's arena loads its own copy of the compiled program …
  after spawn and again after every `resume`" (`vagent-agent-language.md` §4.4) — arena-load is
  precisely the "ephemeral twins" configuration v1.6 relegates to the empty-set arm.
- M11's "program cells therefore do not checkpoint" (`:349`) now needs the not-promoted-vs-carried
  distinction: under the carried-prefix model the image is never *promoted* (V12 intact — `promote`
  walks the arena only) but it *is* carried in the shipped history — the boundary the whole section
  rests on depends on that carriage.

So: **the ordering dependency between program load and genesis is not yet honest.** The honest
statement is one clause at the loader, not a genesis step: "the loader anchors the agent's store on
the artifact (the `job.base` chaining pattern) or appends the artifact's spool before any state op;
the shipped history then carries the image as its low prefix; `rebuildManifold` reproduces it
ref-preserved; genesis asserts the boundary at (c), it does not construct the ordering at (a)."
Plus the corollary edits: M8's "all arena cells" → persistent base cells the arena reads through
and shadows; M11's "do not checkpoint" → "not promoted (carried in the shipped history)"; the
`importValue`/sibling-manifold arm's precondition named.

### 2. MINOR (extent's carrier, bound form and owner unnamed) — RESOLVED in substance; three residual ambiguities demoted to nits

The carrier is named twice consistently — "recorded once at load **on the `programRef` bind
record** — adapter-side durable state, stable across geneses of the same checkpoint" (`:1315-1317`)
and "a field on the adapter-side `programRef` bind record" (`:1589-1591`) — and the adapter
inventory gained the duty: "the `programRef` bind record carries the recorded program-image extent
field genesis (c) asserts (§6.6)" (`:2082-2083`). Residuals:

- Bound at (c), on a record (d) binds: "this step asserts it" runs a step before
  "`programRef → the program root`" is bound — the record is (d)'s product, so either the field is
  written onto a not-yet-bound record (say so) or the steps want reordering.
- "Adapter-side durable state" has the wrong durability class for a cold resume: adapter records
  die with the process. What actually makes the value stable is (i) deterministic re-observation at
  each load, or (ii) the bind record riding the carried image — and the stability guarantee itself
  is `programRef` pinning in the continuation (`mesh_actor.cpp:199,215`: resume refuses a grant
  whose `programRef` differs), which no sentence names. "Stable across geneses of the same
  checkpoint" undersells it — it is chain-stable because the continuation pins the artifact.
- "This step asserts it" still names no comparand; with (a) in place the assert can now be stated
  concretely — "asserts the post-image persistent high-water equals the recorded E".

### 3. MINOR (reflect-born `run` arm guaranteed dead) — RESOLVED

` :1447-1450` drops the arm and states the refusal verbatim per the ruling: a `reflect`-born operand
"is a session-side wrapper with no durable referent, so an opaque or `reflect`-born `run` target in
a learnable fragment is a closedness refusal at emission". The P16 row's rebind shapes no longer
list it, and `patch:` is gone from the label arm (`:1486-1488`, `:2106`).

## New findings (v1.6 deltas only)

### 1. MINOR — the empty-set arm fails open: an unclassifiable image silently voids the whole sideness discipline (§6.6 `:1324-1325`; genesis `:1581-1582`)

**Problem.** "A program image arriving through `importValue` ingress (ephemeral twins) or a sibling
manifold reports an empty program-side set by design, not an error." With program-side = ∅ the
transit-prune never fires; "state-side" degenerates to plain agent-reachability; `closed`'s arm (a)
then admits a capture link into an ephemeral program cell (it is reachable, unpruned, and not
itself program-side — the empty set prunes nothing) — so `learn` accepts a fragment that drags the
entire skeleton into every checkpoint. V12 through the sanctioned path, silently, "by design". And
this is not a distant corner: under the parent spec's stated load model (program loaded into the
arena, §4.4), *every* program is this case — the discriminator is vacuous until the load model is
amended per finding-verdict-1. A safety boundary that reports empty must refuse loudly (a refused
configuration at genesis/`learn`) or name a degraded mode where `closed` means reachability-only —
silently blessing it is the one arm this section cannot afford.

**Fix.** One clause: an image with no persistent program-side cells is a refused configuration for
`learn` (the sideness test exists to bound promotion; an empty program-side set means the bound is
absent) — or state that the arm exists solely to reject mis-anchored images at `learn`-verify time,
not to admit them.

### 2. MINOR — step (b) narrates the `receiveValue` fold as a genesis overlay (genesis `:1582-1586`)

**Problem.** "(b) **arena overlay** — the checkpointed state cells join the persistent numbering
above the extent (persistent positions are stable across the rebuild)". The checkpointed state does
not "join" anything during genesis — it is the received base manifold, folded by `rebuildManifold`
inside `receiveValue` before the actor's `WorkerContext` (and therefore before genesis) exists; the
arena overlay adds only ephemeral runtime cells. In v1.5 "(a) arena overlay" at least matched the
real first event; v1.6's (a)/(b) pair describes a two-phase persistent construction — image fold,
then state join — that no step of genesis performs and the platform cannot perform in that order.
Same root as the persisted major, but the step itself is now wrong on its face and will mislead
whoever sequences the adapter's genesis code.

**Fix.** Rewrite (a)/(b) as inheritance, not construction: "(a) the received manifold arrives
image-below-state by store construction (the loader's anchor); the arena overlays it;" then the
extent assert and `opcodeMap_` re-point as checks/re-materializations of *session-side* state only.

### Nits

- **"The pump orders publish ahead of all queued turns while the flag is armed" (`:1538-1540`).**
  `pumpOne` serves the mailbox and invokes `executor_` (`mesh_actor.cpp:461-476`); it cannot see
  the adapter-private flag. Either publish runs synchronously inside the flagged turn's `executor_`
  window — then no pump ordering exists or is needed, and the sentence should say that — or the
  pump must consult adapter state, a new coupling that contradicts "no new mechanism beyond P4's
  already-approved wake" (`:1505-1507`, `:2106`). As written it promises a runtime guarantee with
  no owner.
- **"Re-`import` the prelude" (`:1575`).** `import`/`importValue` is the ingress verb whose
  products are the ephemeral twins the same sentence excludes; and the prelude is compiled into
  the artifact (linked like `std:agent`, `:937-939`), not imported per load. Say "fold the
  artifact's ops" plainly — the verb matters at exactly the step where a wrong reading produces
  the empty-set case.
- **M8's keyless memo records (`:292-297`).** A label-less attach cell "has no key to write, so its
  wrapper never memo-hits and is re-minted per call" — but does a keyless memo *record* still
  append to the clause's `+d.code` chain per call? If yes, the chain accumulates dead records
  unboundedly (program-side, dies with the skeleton, but O(n) growth on a reflection hot path);
  say which.
- **Carrier residuals** (see verdict 2): the (c)-writes-on-(d)'s-record ordering, "adapter-side
  durable" vs re-observed-at-load, and the assert's unnamed comparand.

## Structural notes (no finding)

- **The name-record writeback split is kind-honest and self-consistent.** Entry-valued vs
  head-valued is determined by the record's `+d.name` *shape* (`:1470-1493`): qualified → symbol
  resolution → `int64(entryOp)` (the §6.1 op-or-head hop preserved at `exportSymbol`,
  `vortex_core.cpp:119-127`); label → program-image lookup → `int64(head)`, matching §6.2's new
  "head-valued by construction" (`:1085-1089` — verified `run` derefs `+d.entry` and `+d.values`).
  Call records always qualified, label records always head-valued, user `#label`s colon-free and
  `src:`/`reify:` label-reserved — the shape test's ambiguity is engineered out on both sides, and
  a non-resolving qualifier degrades to a dropped patch, never a wrong bind. R6-B closed.
- **`code_named` stays `d.code`-only; literal heads take the program-image walk** (`:1655-1660`) —
  the domain split is now consistent end-to-end.
- **The publish-ordering extension covers my r6 nit** — a queued ready-intention/due-timer wake is
  held behind publish (`:1538-1540`); only the "the pump orders" attribution is off (nits above).
- **Step (c) binds the extent before (e) consumes it; (b)'s `opcodeMap_` re-point correctly moved
  ahead of (e)/(f)** — intra-step ordering is sound wherever the steps name real genesis work.
- **The root-exemption clause is well-placed** (`:1335-1336`): under the carried-prefix model the
  agent cell *can* sit at or below E (adapter store writes at spawn precede or interleave the
  artifact prefix), so "a program-side `agent_cell()` cannot prune the durable set" is the
  necessary arm — and its presence quietly confirms the authors know below-extent state exists,
  which makes the unstated store-anchor gap in finding-verdict-1 more conspicuous, not less.
- **Makefile inventory unchanged and correct** — `VAGENT_FRONTEND_SRCS` line untouched (`:2085-2090`);
  the only new inventory line is the extent field on the adapter row (`:2082-2083`).
- **Cites re-verified**: `importValue` re-kinds marked cells `OpHandle`-to-local-twin and demotes
  unmarked `OpHandle` to `None` (`mesh_value.cpp:483-501`); `WorkerContext`'s base is ctor-fixed
  and non-movable (`worker_context.hpp:51-60`); the grant/continuation pins `programRef`
  (`mesh_actor.cpp:199,215-216`) — the stability source the doc should name.

## Verdict

The disposition's wording landed verbatim and the R6-B split is clean. But the headline fix moved
the defect rather than closing it: image-first ref-preserved re-materialization is the right
invariant in the wrong locus — genesis cannot order image-before-state inside a base that arrives
already folded and const, and the load-time store anchor that alone makes the layout true is
unstated and contradicted by M8's "all arena cells" and the spec's arena-load model. Read literally,
step (a) re-inverts the boundary it exists to protect. Clause-fixable — relocate the ordering claim
to the loader, state the store anchor, distinguish carried-vs-promoted in M11, refuse the empty-set
config — but still load-bearing and still unowned.

VERDICT: NOT CONVERGED — 0 blocking, 1 major, 2 minors, 4 nits
