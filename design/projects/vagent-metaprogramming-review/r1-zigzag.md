# R1 — Xanadu / ZigZag Purist critique

Lane: Nelsonian hypermedia and ZigZag topological purity, the strict 2-degree rank invariant,
Store–Slice convergence.

Reviewed against `apps/common/xanadu/zigzag/arena_manifold.{hpp,cpp}`, `manifold.hpp`,
`vortex/vortex_vm.cpp`, `vortex/vortex_core.{hpp,cpp}`, `vortex/mesh_value.cpp`,
`vagent/compiler.cpp`, `design/store-slice-convergence.md` (R1–R12), `design/vagent-agent-language.md`
(V10–V12, §5.1, §5.7) and `design/vortex-hyperstructural-runtime.md`.

The one fact everything hangs on, verified in code: `ArenaManifold::link(from, dim, dir, to)`
maintains the reciprocal-edge invariant by *eviction* — it computes `displacedUs` (from's old
dir-neighbour) and `displacedIt` (to's old −dir-neighbour) and clears both
(`arena_manifold.cpp:1347-1377`). Every cell has exactly one +d neighbour and one −d neighbour per
dimension. A second `link` into an occupied slot does not fan in; it *severs*. The document builds
its entire graft/durability story on `Link`/`Break`/`New`/`Splice`/`Clone` and nowhere states this
rule. Most findings below are the same eviction, landing in different places.

## Findings

### 1. BLOCKING — `+d.grab` is overloaded three ways; the fragment head and every `+d.grab` graft collide with the VM's wing convention (§6.1, §6.3, §6.6, §9.2)

The VM already owns `±d.grab` on an op cell: `+d.grab` is the input wing, `−d.grab` is the output
wing (`vortex_core.cpp:354-414` — `bindInput` links the first operand slot at `opcode +d.grab`,
`bindOutput` at `opcode −d.grab`, slots chained on `+d.step`). The doc then assigns `+d.grab` two
more, incompatible roles:

- **Head→entry attach** (§6.1): `H -(+d.grab)→ entry op`. `link(H, grab, POS, entry)` writes
  `entry −d.grab = H`, evicting whatever held `entry −d.grab` — the entry op's *output wing first
  slot* for any entry with bound outputs. That is the common case: `emitStatement`'s `readDepth`
  carries `bindOutput` (`compiler.cpp:867-868`), and a body's first op is typically an `arithmetic`
  op with `bindOutput` (`compiler.cpp:537-545`). If the wing was occupied it is silently orphaned;
  if it was not, `outputsOf(entry)` now walks `rank(H, d.step)` — the head's unrelated step chain —
  as if it were operand slots. The stated head layout corrupts the graph it names at emission time.
- **Clause→test attach** (§6.3 `graft_where`, `graft_plan`; spec §5.7): `link(clause, grab, POS,
  testEntry)` sets `testEntry −d.grab = clause`, evicting (a) the fragment head's `+d.grab → entry`
  edge — the head is *decapitated*: `run` (§6.2: "jump to `c`'s entry op", found via the head's
  `+d.grab`) can no longer reach the entry — and (b) the entry's output wing, as above. After
  `learn_where(plan_named("Guard.cautious"), code"…")` (§9.2), the recorded head `H` reaches only
  its descriptor and label.
- **Patch→head** (§6.6): `patch +d.grab → H` writes `H −d.grab = patch`; a second patch naming the
  same head (two `learn_*`s sharing one `Code`, or `learn_plan` reusing a body fragment) evicts the
  first patch's `+d.grab` — the earlier patch silently loses its fragment.

The same eviction on `+d.spin`: `graft_body`/`learn_plan` on a `Code` already installed elsewhere —
`link(C2, spin, POS, bodyEntry)` clears `C1`'s `+d.spin` (`displacedIt`), so the first learned clause
loses its body. The doc never says a fragment is single-installation; `Code` values are passed
freely and §8.5 reifies precisely because sharing is assumed elsewhere.

This also falsifies §6.6/§4.5's reachability claim *both ways*. `promote` walks edges **both
directions** (`discover(edge.pos); discover(edge.neg)`, `arena_manifold.cpp:1781-1783`). If the
graft decapitates `H`, the patch's `+d.grab→head` no longer reaches the graph — the fragment is
unreachable from state and absent from the checkpoint (V12 stands for program cells). If instead
the head→graph edge survives *and* the graph is installed, the bidirectional walk goes
`patch → H → entry →(−d.grab/−d.spin)→ clause → goal → module → home`: **the whole program skeleton
is dragged into the checkpoint — precisely the failure V12 exists to prevent and the M11 amendment
claims to scope away.** The only consistent `learn` is: graft a *copy*, keep the *original*
uninstalled and closed, patch-record the original. That requires the single-installation rule and
`reify`-per-`learn`, neither of which the doc states.

**Fix.** (a) Attach fragment *heads* — non-op indirection cells, Vlog's clause-head shape — at graft
points (`clause +d.grab → H`, `clause +d.spin → H` or its entry; heads have no wings, so nothing is
evicted). (b) Move the head→entry edge off `+d.grab`: a dedicated data-minted dim (`d.entry`,
adapter-minted by the same V10 mechanism as `d.code`/`d.patch`) or `+d.step`/`+d.spin` following the
`attachPipeline` idiom (`vortex_core.cpp:438-441` — a non-op cell pointing into a stream is already
how param pipelines head a chain). (c) Put `patch→head` on a dim disjoint from the attach edge.
(d) State the rule: a `Code` denotes one installation; graft consumes it (or `graft_*` reifies
internally); `learn_*` always keeps an uninstalled closed fragment for the patch.

### 2. BLOCKING — a promoted fragment graph is dead on arrival: operand cells are not link-reachable and wing slots are verbatim Int64 (§4.5, §6.6, M11, §12)

Vortex operand wiring is ref-box `Int64` scalars: `bindInput`/`bindOutput` mint
`makeScalarCell(int64(operand))` (`vortex_core.cpp:384, 402`), `reference()` mints
`makeScalarCell(int64(target))` (`compiler.cpp:516-518`), and `Linked` materializes the same
ref-boxes (`vortex_vm.cpp:706-707`). Two consequences the doc misses:

1. **`promote` reaches cells through links only** (`arena_manifold.cpp:1765-1784`). Literal cells
   (`literal()`, `compiler.cpp:559-569`), temporaries, and the per-emission `cells_` helpers
   (`compiler.cpp:850-852`) are referenced solely by Int64 slots — no link reaches them — so they
   are *not in the checkpoint at all*. The doc's `closed` flag is name-closure ("outside references
   are state-side binding cells"); a fragment's internal literal/reference cells are also outside
   references, to non-state cells, so no `code"…"`-born fragment is closed in the link-reachability
   sense `promote` implements.
2. **Int64 bits promote verbatim** (`store.makeScalarCell(out.version, from.asInt64(arena)…)`,
   `arena_manifold.cpp:1934-1936`) and `importValue` likewise copies value bits verbatim,
   translating only `ValueKind::OpHandle` relocation handles (`mesh_value.cpp:483-491` vs.
   `:494-502`). After `resume`, every wing slot and every `Jump`/`Call`/`Branch`/`Choice` target in
   the fragment is a stale dense id — the doc's own §4.5 table calls such ints "meaningless: stale
   or aliased", without noticing the fragment graph is built out of them. `resolveCell`
   (`vortex_vm.cpp:60-67`) then answers absent-or-wrong cells for every operand.

So "re-runs `learn`'s graft per patch after resume" re-installs a graph that cannot execute, and
§6.7's "`learn`'s links are structural and suffer nothing" is true of the links and false of the
referent.

**Fix options, pick and price one.** (a) Emit fragment operand slots and `reference()` targets as
`OpHandle`/relocation-handle cells — the one value kind `promote` and `importValue` *do* translate
(`arena_manifold.cpp:1759-1763, 1947-1951`; `mesh_value.cpp:494-502`). That is a change to emission
code (`compileFragment` new code at minimum; `bindInput`/`reference()` if shared — register as
P16 beside P15). (b) Admit `learn` cannot carry executable graphs and have the patch carry a
re-emission recipe — but "recompile at load" is refused in §11, so this means re-defining the patch
as a *builder fragment*, which has the same Int64 problem. (c) Narrow `learn` to session scope and
say so. As specified, M11 cannot work.

### 3. MAJOR — `d.synapse` topology cannot represent fan-in, and its endpoint encoding is unspecified (§2 synapse, §6.1 `sprout_synapse`, §8.1, §8.3)

"A `d.synapse` link between two cells — pattern cell → clause cell" gives each endpoint exactly one
`±d.synapse` neighbour: the second `sprout_synapse` into a target evicts the first, and likewise out
of a source. A neural structure without convergent connectivity is not one. Meanwhile the worked
example iterates `cells"$a/+d.synapse"` — synapses filed as a rank off the *agent*, i.e., junction
cells — a different shape than "a link between two cells". If junction cells: endpoint edges collide
again (`S +d.grab → pattern` evicts `pattern`'s `−d.grab`, including its wing/attach edge), unless
endpoints are ref-box Int64s (session-only handles, §4.5's own caveat) or names (V11's discipline).
Nor is durability addressed: a synapse junction linking to a clause cell drags program structure
into the checkpoint under `promote`'s bidirectional walk — the M11 amendment scopes only *closed
fragment graphs*, not synapses. And weights "on a clause's `d.vars`" or a synapse cell's `d.vars`
live on program-side or junction cells — a weight on a `learn`ed clause dies at `resume` like the
clause does.

**Fix.** Specify the synapse shape: junction cell on the agent's `+d.synapse` rank (which is what
§8.3's iteration already assumes), endpoints as *qualified-name* or label cells (durable, no
eviction, V11-consistent) or as `d.clone` instances (transclusion-honest, but drags endpoints —
needs its own V12-scoped ruling); state whether synapses/weights are session-only or durable, and
re-home durable weights onto state-side cells (patch, junction, or agent `d.vars`).

### 4. MAJOR — `graft_method` "callable by name immediately" is false for redefinition (§6.3)

`exportSymbol` appends a symbol cell at the `+d.vars` rank tail (`vortex_core.cpp:119-127`) and the
name walk (`findModule`/`symbol_named` style, `vortex_core.cpp:100-106`) returns the *first* match.
Grafting `owner.name` where `owner.name` already exists mints a second same-named symbol cell that
is shadowed forever by the original — the grafted method is unreachable by name.

**Fix.** `graft_method` must first unlink an existing `d.vars` symbol of the same name (the
predecessor→successor repair `prune` already uses), or the doc must state a last-match-wins lookup —
which inverts the spec's declaration-order reading and should be refused. Same dedupe rule applies
to `learn_plan`/`graft_plan` clause `name` labels if plan selection resolves first-match.

### 5. MAJOR — `reify`'s mechanism is unspecified, and the obvious tool is the wrong one (§6.1 table, §8.5)

The `Clone` opcode is in the surgery list, but `Clone` does not copy: it appends a cell to the
source's `d.clone` rank (`vortex_vm.cpp:807-836`), and `readValue`/`writeValue` resolve through
`masterOf` (`vortex_core.cpp:181, 202`) — content and value bits are shared with the master. A
"reified" clone shares the original's op kinds and operand bits: it is a transclusion-instance, not
a copy; mutating its values mutates the original, and its `closed` claim is unverifiable.
"Compile-assisted" hints at re-emission (re-run the fragment's emission against the recorded
AST/descriptor, minting fresh cells and fresh refs) — the only honest deep copy here, since it
remaps every Int64 by construction.

**Fix.** State that `reify` re-emits from the fragment's recorded emission data (new local cells,
fresh refs, descriptor re-minted) and name `Clone` as refused for it — with the reason (clone shares
content/value through `masterOf`; a "copy" that aliases is not a copy).

### 6. MINOR — source provenance is a coordinate string, not a transclusion (§5.1, §6.7, M1 price)

`+d.name → "src:demo:12:4"` serializes a source position into scalar text. It is not a
transclusion: the fragment's source bytes are not addressed, and nothing states that the program
artifact or any scroll stores the `code"…"` body the coordinates name. §12 honestly disclaims
source recoverability, but the purist contract deserves the sharper statement: the label is a
*name* requiring an external source map; if the artifact sealed the literal body as a span (or the
source file is itself a stored scroll), the label could be a `GlobalSpan`-style reference rather
than a string. Record which.

### 7. MINOR — descriptor type identity is stringly-typed (§5.2, §5.4, §6.1, §7.2)

Parameter/result types are stored as type-name strings (`"Int"`) on `+d.values`. There is no
canonical form (`Code<Int>` nesting, qualified names, renamed types across incarnations), so the
runtime descriptor comparison in §7.2 compares surface text — a small foreign serialization inside
cells, mildly at odds with M1's no-side-representation spirit (defensible: type cells vs. names is
a real trade — argue it rather than assume it).

### 8. MINOR — `learn_plan`'s patch records one `+d.grab→head` for a two-fragment graft (§6.6, §6.1 patch row)

`learn_plan(goal, name, test, body)` installs two fragments; the patch shape `{ordinal, target
descriptor, +d.grab→head, +d.name→label}` names one head. Which head rides the patch — test, body,
or a record cell chaining both (e.g., `+d.step`)? Genesis replay needs both.

### 9. MINOR — `validate`'s "seen-set like `Linked`'s" has no Vortex mechanism named (§7.2)

`Linked`'s cycle guard is a C++ `unordered_set` (`vortex_vm.cpp:699`); `std:meta` is Vortex code
with no set. A bounded walk needs mark-links (a fresh `d.validate`-mark dim — safe, since a fresh
dim has no occupant to evict — trailed and cleaned) or Floyd's tortoise-hare over the spin chain.
Say which; "bounded, cycle-guarded" is asserted, not priced.

### 10. MINOR — two unpriced residues (§5.5, §6.4)

- `static e` runs `vm_.run` in the kernel's arena — the same arena the artifact is being emitted
  into. The run's cursor, frame and temporaries are arena cells that persist in the emitted program
  as dead cells; §13 prices fragment heads but not static-run residue. Emit in a scratch region
  released after read, or price the residue.
- §6.4's "ahead of the PC" story covers sequential `+d.spin` advance, but `Jump`/`Branch`/`Call`/
  `Choice` targets are Int64 ref-boxes (`vortex_vm.cpp:839-873, 1150-1168`) — int-addressed control
  flow still reaches unlinked ops. Worth one sentence: pruning does not stop int-addressed entry,
  only chain advance; containment remains `maxCycles` + `release(m0)` as stated.

## Verified accurate

Citations checked and correct: `mintOpcode`/`opcodeMap_`/`getOpcodeKind` ceiling
(`vortex_vm.cpp:73-114`), `setCursorOpcode` trailed PC (`:131-146`), structural ops
(`:616-836`), `Linked` seen-set (`:699-701`), `New` already-used refusal (`:736-743`), `Choice`
op-target enforcement (`:1150-1158`), trail/shadow machinery (`arena_manifold.cpp:1506-1610`),
`compact()` refusal under marks (`arena_manifold.hpp:670-679`), `cells"…"` subset
(`cell_path.cpp:147-183`), `ensureDimension`/V10 discipline, R5/R6/R7/R8/R12 reconciliations (§14) —
all sound. `discard`'s last-mark trail clear matches P13 (`arena_manifold.cpp:1606-1608`). The
prune predecessor→successor idiom is exactly the one link call that exploits the eviction rule
correctly (`insertIntoRank`, `arena_manifold.cpp:1423-1439`).

VERDICT: NOT CONVERGED
