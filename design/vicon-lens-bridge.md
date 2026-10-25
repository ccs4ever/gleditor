# vicon Lens Bridge: Lenses as Suspended Host Requests in a Goal-Directed Language (v1.2)

A design specification for how a **vicon program** — not a `vicon` REPL verb — opens, traverses,
mounts, writes back, and releases projection lenses. Design deliverable only: the signatures are
proposals; the `write` builtin's produce-and-suspend protocol and the `session_ops` gesture layer
are the seam, not a new layer. The sibling specs are
[`vql-lens-bridge.md`](vql-lens-bridge.md) v6.6, [`vpl-lens-bridge.md`](vpl-lens-bridge.md) v1.4,
and [`vlace-lens-bridge.md`](vlace-lens-bridge.md) v7.4 — the capability set is the same; only the
surface is re-thought for a compiled, goal-directed language.

vicon is **compiled, not interpreted**: every expression lowers to an inline VortexVM opcode graph
(`compiler.cpp` — each node emits `Jump`/`Branch`/`Choice`-linked cells on `+d.spin`), produces by
writing a result cell and landing on a shared `Halt`, and resumes by backtracking
(`compiler.hpp:11-22`). There is no step-dispatch table to extend (contrast VQL's `evaluateStep`)
and no `IO a` value type to carry effects (contrast vlace). The only host-effect channel is
**produce-and-suspend**: `write`'s tee (`compiler.cpp:696-722`) is the one builtin that reaches the
host, and it does so by suspending on `haltOp_` with a value the Driver recognizes. The design
center therefore shifts again — from VQL's "where may a mutating step appear" and VPL's "which
operand positions carry a mutating quad" to **how a compiled generator talks to the Driver across a
suspension, and what Icon's re-drive semantics mean for a session effect**.

**v1.1 folds the R1 review** (ergo/vortex/purist): the cell currency is declared — **Int64
ref-tokens** (D1); the arena half of every session write is trailed and backtracks, so session
truth must carry the payload (D4); stale refs *alias*, not just dangle, so the repair is epoch
invalidation, not `contains()` (D5); `&lens`/`&lenserr` are produce-time reads of Driver-held C++
state, not stored cells (D2/D3); the write funnel sits inside `ArenaManifold`, not `VortexCore`
(D7); and `Driver::start()` must drain the previous program's marks (D2/D5).

______________________________________________________________________

## 1. Intent

A vicon program should be able to write, in its own terms:

- **open a source** as projected root cells — `dir:`/bare path, `git:`, `sql:`, `cfg:`, `log:`,
  `journal:`, `union:` — the `openLensUri` scheme table (`lens_uri.cpp:23-125`);
- **traverse** with the generator idiom it already has — `kids(c)` producing children one at a
  time, the way `i to j` produces integers;
- **mount a delegate** under a projected cell and come **back** — "back" is a bound variable;
- **edit, commit, and revert** projected cells on write-capable lenses (`sql:`/`cfg:` today);
- **materialize** a deferred page, and let sessions go out of scope.

Acceptance journey: `open dir → kids() listing → mount git under the repo cell → walk commits →
unmount → keep walking → open sql: → browse → edit+commit → revert a refused edit → materialize a
page → program ends`.

## 2. Current surface (already in the build)

| surface | what it does |
|---|---|
| `CellValue` | `variant<string,double,int64_t,bool>` — **no `CellRef` alternative** (`vortex_core.hpp:29`): a cell crosses a value boundary only as rendered content or as an `int64` |
| produce / `Bind` / `Resolve` | copies *rendered* values — `emitProduce`'s `Value` op (`compiler.cpp:89-101`), `Bind`'s `core_.value(valCell, 0, -1, inputs[1])` (`vortex_vm.cpp:711-715`), `Resolve`'s `core_.render` (`:744-750`). A bound variable holds whatever `render()` produced, never the cell |
| the int64→CellRef idiom | `opref` encodes a jump target as `int64(cell)` (`compiler.cpp:80-82`); `resolveCell` decodes `int64→CellRef` with a `contains` check (`vortex_vm.cpp:48-64`); `Deref` emits `int64(cellRef)` (`:929-942`); `wingOf`'s "Int64 naming a cell" operand (`vortex_core.cpp:337-352`); `MakeVar` emits `int64(var)` (`vortex_vm.cpp:886-894`). **Refs already travel as Int64 throughout the VM** |
| `write(e)` | the sole host-effect builtin — `emitCall` special-cases it: tee the produce to `resultCell_`/`haltOp_`, then forward to the consumer on resume (`compiler.cpp:696-722`) |
| `Driver::resume()` | the suspension protocol: `goalProduce` spawns the cursor under the sentinel choice point; `goalResume` lands it on `failOp_`; **both require `pc == haltOp`** (`driver.cpp:32-52`, `vortex_stdlib.cpp:3738-3776`) — a stop on any other `Halt` reads as failure and unwinds the activation's choice points |
| `every` / `start` / `runAll` | the Driver API (`driver.hpp:38-67`); **one `Driver` per process** (`apps/vicon/main.cpp:121`) owns `arena_`, `core_`, `compiler_` (`driver.cpp:15`). `start()` (`driver.cpp:17-30`) replaces the program but **never drains the choice stack** — the previous program's marks stay outstanding |
| `x := e` | `Bind`/`Resolve` on the cursor's `+d.vars`/`+d.values` scope (`compiler.hpp:19-21`; `vortex_vm.cpp:705-750`) |
| `&name` | keyword primary — lexed generically (`lexer.cpp:244-256,:275-277`), emitted for `&null`/`&fail` only (`compiler.cpp:194-202`); `&lens` needs no lexer or parser work |
| `f(args)` | `ECall` — builtins dispatch **before** `procTargets_` (`compiler.cpp:696,:724-728`), so a builtin name silently shadows a user procedure of the same name; unknown names are `CompileError::UnboundIdentifier` |
| `guardedAlt(alt)` | wraps a resume path in every active `\n` kill-gate (`compiler.cpp:125-136`) — every `altOp` the compiler emits goes through it |
| `opcodeMap_` | the VM's opCell→kind map (`vortex_vm.hpp:176`, written at `vortex_vm.cpp:70-95`) **accumulates across `compile()`s** — stale kinds alias after dense-index reuse |

vicon has **no lens surface at all** — no `:lens` REPL verbs, no `LensRepl`, no session registry, no
traversal verb (`!x` and `s[i]` are parsed but unimplemented, `compiler.cpp:203-211,:271-275`). The
bridge introduces every piece.

## 3. Design decisions

### D1 — Gestures are suspended *requests* on a second `Halt`; the currency is the Int64 ref-token

The vicon spec's §8.1 argument — "no ninth opcode, no new `ChoicePoint` field" — holds: the
`OpcodeKind` table (`vortex_vm.hpp:27-87`) gains no `Lens*` entries. Instead a lens builtin emits a
**request fragment**, generalizing the `write` tee: `Value` ops write the request record
(`__vicon_lens_op` = gesture id, `__vicon_lens_a1`/`_a2`/`_a3` = arg cells — three, so future verbs
are not hard-capped at arity 2 — `__vicon_lens_cont` = `opref` of the site's continuation), then
`Jump lensGateOp` — a second `Halt` minted per program alongside `haltOp_`/`failOp_`
(`compiler.cpp:782-783`). `vm_.run` stops on any `Halt`; the Driver's `resume()` loop distinguishes:
`pc == haltOp_` → deliver `resultCell_`; `pc == lensGateOp` → decode the record, run the
`session_ops` call **host-side**, write `__vicon_lens_out` (answer cell) and `__vicon_lens_ok`
(0/1), `setCursorOpcode(cont)`, and run again — all inside one `resume()`, so a request never
surfaces as a program result (`driver.every`'s sink sees only real produces, `driver.cpp:54-58`).
The site's continuation is `Branch ok → produce out`, else `Fail`.

**The currency declaration.** `CellValue` has no `CellRef` variant, and produce/`Bind`/`Resolve`
copy rendered *content* (§2) — so a cell cannot cross a suspension or a variable boundary as
itself. The only coherent currency is the one the VM already uses: **the Int64 ref-token**
(`int64(cellRef)` — the `opref`/`resolveCell`/`Deref`/`wingOf`/`MakeVar` idiom, §2). `r :=
lens("dir:/x")` binds the Int64 dense index; `kids(r)` decodes `r`'s value `toInt → CellRef`
host-side. Every consequence is absorbed:

- **`write(c)` prints dense ref-ints** — `render()` stringifies the scalar (`driver.cpp:69-82`), so
  `every c := kids(r) do write(c)` prints `42`, `43`, …, not names. Display is a verb:
  `ltext(c)`/`lshow(c)` (§4) are **required** for §5 to print anything readable.
- **`=` on two ref-int values IS identity** — `Eq` compares rendered values, and a ref-int's value
  is the index. `leq` is near-redundant; it survives only as the *validated* identity (below).
- **Ref-ints are untyped.** `kids(42)` — or `kids("42")`, `toInt` runs `stoll` on strings
  (`vortex_vm.cpp:41-45`) — silently resolves dense index 42: the `LiteralCellId` wrong-answer
  channel the VQL doc guards (`vql-lens-bridge.md` D2). Rule: **every request arg is validated**
  `toInt → CellRef → arena().contains() → epoch check (D5) → sessionFor/ownsCell` where the op is
  session-scoped; a failing arg makes the request `Err` (Icon failure), never a wrong answer.
  `c + 1` still adds happily — arithmetic cannot tell a ref from an integer (`MakeVar` has the same
  wart, `vortex_vm.cpp:893`); documented, not fixed — it is the `opref` idiom's own property.
- **Scalar answers collide with ref-tokens.** `ldirty`/`lcommit` producing `42` is
  indistinguishable from a produce naming cell 42. Accepted: the producing verb declares the kind
  positionally — the program knows `ldirty` yields a count and `kids` yields refs. A tagged
  currency would close it at the cost of `CellValue` churn (§6.8).
- **The Driver never mints at gate time.** Every answer is a `Value` write into the compile-minted
  `__vicon_lens_out` cell — a gate-time mint lands above live choice-point marks and dangles on
  backtrack (D5). The request cells are compile-minted, so Driver writes to them are trailed like
  any other write and backtrack cleanly; the session effect they describe is the problem D4 owns.

On `expected`-refusal the Driver writes `ok := 0` and records the reason in Driver-held state —
the call **fails** (D3).

`lens(uri)`, `kids(c)`, and `lroots(c)` produce **several** results: the Driver holds a per-site
answer queue (`unordered_map<site-id, deque<CellRef>>` — the request carries a compile-minted site
scalar). Queue discipline: **a fresh request at a site REPLACES the queue** — a re-fired
`kids(c)`/`lens(uri)` with a different arg must not serve the old arg's stale answers, and a
re-drive must not append duplicates. The fragment's `altOp` re-sends `NEXT site` — wrapped through
`guardedAlt` (`compiler.cpp:125-136`) like every other resume path, so a `\n` kill-gate poisons the
NEXT re-send (the flag's non-persistence is the inherited I2 deviation, D4) — the queue empties,
the next `NEXT` writes `ok := 0` → Icon failure. One host round-trip per produce, lazy by
construction. Queue pops validate `contains()` + epoch (D5).

`lens`/`kids`/`step` and every `l*` builtin name is **reserved**: `emitCall` checks builtins before
`procTargets_` (`compiler.cpp:696,:724-728`), so a user procedure named `kids` would be silently
shadowed — declaring one is a `CompileError`, not a shadow.

### D2 — Sessions live on `VortexCore`; `&lens` and `&lenserr` are produce-time reads

The Driver is process-lived, so a core-held registry survives `start()` replacing the program — the
**same `lensSessions()` row VQL's Q4 registers** (arena-level `weak_ptr` membership + owning
`vector<shared_ptr>`), scanned by `sessionFor`. **Open dedupe by canonical source identity**:
`lens("dir:/x")` under a live session re-binds the existing roots; two sessions over one writable
source would split `dirty_` and cross-clobber on `commit()` (`keyToCell_` is per-session,
`projection_session.hpp:200-201`). `openLensUri` does **no** normalization — `dir:/x`, `/x`, and
`./x` open three sessions over one directory (`lens_uri.cpp:104-124`); the registry records the
canonical identity (filesystem paths canonicalized, scheme spelled once), so dedupe is honest on
every open path.

`&lens` and `&lenserr` are **produce-time reads of Driver-held C++ state**, not cells the Driver
writes — the `&pos`/`&subject` precedent (`scanPos`/`scanSubject` are live reads,
`vortex_stdlib.hpp:383-402`), and a necessity: a stored `lensErrCell_` written under the failing
arm's mark is **released by the very `Fail` that reaches the recovery arm** — `(lmount(c) |
write(&lenserr))` would read the *pre-refusal* value, and a stored `lensRootCell_` inherits both
that trail-revert and the D5 aliasing hazard. So the Driver keeps `lastLensSession_` (a
`weak_ptr`) and `lastLensErr_` (a `string`); `&lens` resolves `session->roots().front()` **live at
produce time** — `roots_` recomputes per `refresh()` (`projection_session.cpp` `recomputeRoots`),
so the open-time root would go stale-but-alive — validating `weak_ptr` expiry, `contains()`, and
the D5 epoch; `&lenserr` produces the recorded reason. Both yield `&null` when clean.

**`start()` must drain.** Today `Driver::start` never touches the choice stack
(`driver.cpp:17-30`), and `backtrack`'s foreign-cursor pop drops the frame *without* touching its
mark (`vortex_vm.cpp:198-216`) — so `outstandingMarks_` never returns to 0 across programs:
`compact()` is refused forever (`arena_manifold.cpp:1699-1701`), `ProjectionSession::rewind`'s
foreign-mark check fails permanently, and the trail grows monotonically. `start()` therefore
drains before compiling — releases every outstanding mark in stack order (undoing the abandoned
program's writes; session cells minted under those marks truncate, which the D5 epoch invalidation
absorbs) — resets `opcodeMap_` (stale kinds alias after index reuse), clears the per-site answer
queues, and re-seeds the Driver's `&lens`/`&lenserr` state from the live registry.

### D3 — Refusal is Icon failure; `&lenserr` carries the reason

Every `session_ops` verb returns `std::expected`; the gate maps `Err` to *failure*, not an error
value — `lmount(c)` inside `every c := kids(r)` simply prunes that cell; `(lmount(c) |
write(&lenserr))` recovers — **and reads the actual reason**, because `&lenserr` is the D2
produce-time read, not a trailed cell; `if lmount(c) then …` guards. This is vlace's `Result
String` translated into goal-directed idiom, and it is strictly more honest than VQL's
refusal-is-empty relation: a refused gesture cannot be mistaken for an empty walk. Icon's
`&errortext` shape.

### D4 — Re-firing is Icon's effect contract — and the arena half of a gesture write is what backtracks

VQL needed a static positional rule because its relation re-evaluates *silently*. vicon's rule is
simpler: **a request fires each time control passes through its call site** — every re-fire has a
written generator behind it (a loop back-edge, an alternation arm, a re-driven sequence element —
`{1 to 3; ledit(c,"x")}` edits three times — a re-called procedure); there is no
predicate/join/`return` hidden multiplicity.

But "no session effect is undone on backtracking" is only **half true**. `editContent`,
`restateContent`, and `tombstone`'s `unlink`s write slots **older than the innermost mark** →
trailed (`arena_manifold.hpp:711-720`) → **replayed away** on `release()`
(`arena_manifold.cpp:1653-1657`). The arena image of every gesture — edit text, restated content,
tombstone severances, cells minted above the mark — backtracks; the lens-side truth (mounted
delegates, browsed queries, `applyContentChange`'s source writes) does not. The repair is that
**session truth carries the payload in C++, not in cells**: `dirty_` becomes
key→pending-text (`unordered_map<ExternalKey,string>`) — `commit()` applies
`dirty_[key]` to `applyContentChange`, never `arena.textOf(*cell)` — otherwise `every c := kids(r)
do ledit(c,"x")` followed by `lcommit` would push the **reverted** bytes to the source while
reporting `applied = N` (`dirty_` keeps the key; the cell's content was restored). The
projection's arena cells are a disposable view; a post-release `lrefresh` re-derives them from the
lens.

Auditing the gesture set under re-fire — the arena image reverting on backtrack makes "did the
second fire corrupt" a question about session truth, not cell truth:

| op | second fire |
|---|---|
| `lens` | re-binds via canonical-identity dedupe — benign; the site queue is **replaced** (D1), so a re-fire re-enqueues fresh roots rather than appending |
| `lmount` | **idempotent** — `isMounted` short-circuits before the dirty-gate (`session_ops.hpp:62-66`) |
| `lunmount` | no boundary → `Err` → fails — a re-driven path simply prunes |
| `ledit` | re-writes `dirty_[key] := text` — idempotent in session truth *while the ref is live*; **non-idempotent under D5 truncation**: the arena write is already undone and a stale ref can alias a foreign cell — the epoch check (D5) must precede every fire |
| `lcommit` | applies the pending tail — **non-atomic**: `commit()` returns a refusal with ≥0 already applied (`projection_session.hpp:131-138`, `.cpp:355-388`) — a "fail" that wrote |
| `lrevert` | non-dirty is a true no-op (`session_ops.hpp:165-171`) |
| `lbrowse` | **re-fires mint a second query** — `browse()` mints unconditionally (`sql_lens.cpp:442-466`); `isBrowsed` is a read probe (`sql_lens.hpp:124`), not a guard — the Q6 `sessionBrowseAt` row must gate on it |
| `lnext` | materializes one more page — consumption, the honest cost of re-drive |
| `lstub`/`ldirty` | const reads — safe |
| `lrefresh` | re-runs `refreshIfChanged` — **not** const: it mints, restates, and tombstones; it is only dirty-gate-*exempt* |

So **no positional ban**: there is no position a static rule could call safe (a shared
continuation downstream of any `#Choice` re-fires), and the ops that could corrupt are already
idempotent or self-refusing in *session truth*. Named residues: `lnext` over-consumes on re-drive
(the `every`-driven `lnext(c) \ n` shape is the intended paging idiom); `lbrowse` duplicates until
the Q6 gate lands; `lcommit` can refuse with ≥0 applied — a `&lenserr` reason plus `ldirty` still
nonzero is the honest report; a backtrack between `ledit` and `lcommit` reverts the arena edit but
keeps `dirty_[key]` — the next `lcommit` still pushes the pending text (session truth, by design);
and the broken `e \ n` kill-flag (`compiler.cpp:12-22` — trailed flag writes cannot persist) means
a gesture inside `\n` fires past its bound — the request's `guardedAlt` wrapping only aligns its
re-send path with the (non-persisting) gate mechanism, the same deviation I2 already carries for
`write`.

### D5 — Session mints live above the VM's marks: truncation, *aliasing*, and the epoch repair

vicon is the only frontend whose gestures run **while cursor choice-point marks are outstanding**.
A mount/browse/`refresh()` mints session cells above the enclosing marks' floors; a later
backtrack that releases one of those marks truncates `slots_` to `m.cellCount`
(`arena_manifold.cpp:1665`). Two failure modes, not one:

- **Dangle**: `keyToCell_`/`cellToKey_`/`roots_`, the Driver's answer queues, and any `&lens` state
  name dead `CellRef`s.
- **Alias**: a `CellRef` is `ephemeralBit|dense`; `release()` does `slots_.resize` and subsequent
  mints **reuse the freed indices**. A stale entry then *passes `contains()`* and silently names a
  live foreign cell — `ownsCell` wrong-positives, `ledit` writes one cell's text to another key's
  pending edit, an answer queue serves a re-minted unrelated cell. **`contains()` cannot be the
  repair.** `cellToKey_.emplace` on re-mint makes it worse: it silently keeps the stale reverse
  binding (`projection_session.cpp:121-122,:240,:399` — `emplace`, not insert-or-assign).

Rule: **epoch invalidation**. `ArenaManifold` bumps a `releaseEpoch()` (generation counter) on
every `release()` truncation; every ref-holding structure stamps the epoch at record time — the
session's `keyToCell_`/`cellToKey_`/`roots_`/`presentKeys_`, the Driver's per-site queues and
`&lens` seed. A stamped read at a different epoch **invalidates wholesale**: the session clears
its maps and re-projects on the next `refresh()` (same key → deterministic re-mint); the Driver
flushes the site's queue and re-derives `&lens` from the live registry. `cellToKey_` re-mint uses
insert-or-assign so a rebound index updates the reverse edge. The projection degrades to
"unprojected, refresh to re-see" rather than aliasing. `refresh()`'s own internal mark is the
innermost and nests legally (`projection_session.hpp:96-97`); the hazard is the *outer* release —
which `start()`'s drain (D2) also triggers, so `lcell(key)` — re-resolve key→ref against live
sessions — is the handle that survives an epoch boundary; refs crossing `start()` survive only
because the arena does, and only while no drain truncates them.

This is the vicon-era analogue of vlace D4's "sessions die with the Compiler": sessions outlive
their mints' marks. (The compile-minted flag question it raises is already confused in-tree —
`compiler.cpp:12-22` says writes to compile-time cells *are* trailed while `emitIf` at `:568-573`
says they are not; `arena_manifold.hpp:711-720`'s older-than-the-mark rule settles it: they are
trailed, and the if-guard only survives because `init` re-runs per activation.)

### D6 — `session_ops` carries the gesture protocol; `union:` is key-level; `meta:` refused

The builtins call `sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/
`sessionEdit`/`sessionRevert`/`sessionDirty` (`session_ops.hpp:42-171`), inheriting the dirty-gate
(edit/commit/revert exempt), `canApply` inside `editContent`, and `commit()`→`expected<int>` —
**non-atomic**, a refusal reports with ≥0 applied. Cell→session resolution is the `ownsCell` scan
(node O(1), attr one negward hop, `projection_session.hpp:49-60`); key-needing ops resolve
`keyFor` and fail unkeyed.

**The mount-scoped ops** (`lnext`/`lstub`/`lbrowse`/`lunmount`) route cell → `keyFor` →
`mountBoundary` (`router_lens.hpp:70-71`) → `windowedFor`/`nextStubAt` through the **proposed**
shared resolvers — `sessionMountScopeAt`, `sessionBrowseAt`, and the `SqlLens` root-key accessor
are **VQL's Q6 register rows, not existing code** (`vql-lens-bridge.md` Q6); `claimedBy` is Q4's.
Scope rule: on a `RouterLens` session a cell outside every mount — `mountBoundary`→`nullopt` —
**refuses** (vlace and VQL pin the same); on a plain `WindowedLens` session (`sql:`/`log:`/
`journal:`/`git:` opened directly) **the session is the scope** — `sessionNextPage`'s nullopt arm
(`session_ops.hpp:128-134`) answers session-wide, no mount needed.

**Session handles, not operands**: `lcommit`/`lrevert`/`ldirty`/`lrefresh` take `c` purely to name
the session — `sessionFor(ownsCell(c))` — and the effect is session-wide (all dirty keys, the
whole lens). The per-cell signature is the resolution idiom, not the arity of the effect.

`union:` supports key-level ops only (`UnionLens` is neither `RouterLens`/`WindowedLens`/
`SqlLens`, `union_lens.hpp:31`) — the scoped verbs fail, and the duplicate-`ExternalKey` refusal is
the proposed `claimedBy` probe (first-success write vs last-emission display,
`union_lens.cpp:76-93`). `meta:` stays refused in `openLensUri` (`lens_uri.cpp:33-50`) — the Driver
is store-less like vlace's evaluator; no `MetaSource` fold exists to borrow. xuzz keeps its own
four gesture copies (`xuzz_app.cpp`) and does not call `session_ops` — gate-free, noted, out of
scope (`session_ops.hpp:22-25`).

`lens(uri)` keeps the URI form: a string source designator **is** the Icon idiom — `open(filename)`
— where the siblings took typed spec objects; the URI's scheme already selects the lens type. The
price paid either way is canonical-identity dedupe (D2): normalization lives in the registry open
path, not in the argument's shape.

### D7 — The write funnel is inside `ArenaManifold`, not `VortexCore`; vicon's own surface is scope-wiring

v1.0 enumerated the funnel wrong. `core_.link` is a thin wrapper over `arena_.link`
(`vortex_core.cpp:128-167`), and the compiled surface writes the arena **everywhere**: `New`
(`vortex_vm.cpp:563-584`), `Clone`'s raw `arena().makeCell`+`arena().link` (`:624-633` — bypassing
`core_.link` entirely), `Bind`'s direct arm `core_.value` (`:713`) plus its `arena().link` scope
wiring (`:715-725`), `Call`/`Return` stack frames (`:665-703`), `MakeVar`/`MakeTerm`/`Unify` Vlog
writes (`:855-927`), `spawnCursor`/`setCursorOpcode` (`:122-144`), and `cloneGenerator`'s raw
`arena_.link` (`vortex_core.cpp:317-325`) — plus the compiler's own `spin()`/`bindInput`/
`bindOutput` mints at build time. So the `sessionFor` guard — VQL's Q7B row, which the siblings
just moved — lives **inside `ArenaManifold::link`/`linkChain`/`setValueBits`**, the single
boundary every writer crosses (`arena_manifold.cpp:844-846`,`:1300`,`:1312` trail there);
session-internal writes (`refresh()`/`restateContent`/`writeAttribute`/`tombstone`) are exempt via
a `ProjectionSession`-set inside-session-write flag. That covers every opcode any vicon construct
emits, present and future (`!x`, subscript, `zzLink`-class verbs — I3+).

`Bind`'s scope wiring stays inside the funnel too but is *legitimate* under it — `r := lens("…")`
links the session cell negward on `d.values`, a trailed write that self-cleans on backtrack — the
funnel governs writes **to session-owned cells**, not writes that *name* them (the guard's check
is `sessionFor(target-cell)`); `ledit` is the content-write path.

## 4. Proposed surface

### Keywords — produce-time reads of Driver state (D2)

| keyword | produces | notes |
|---|---|---|
| `&lens` | the most recent *live* session's first root's ref-token; `&null` when none | resolves `session->roots().front()` at produce time — `weak_ptr` expiry + `contains()` + epoch validated |
| `&lenserr` | the last refusal's reason; `&null` when clean | `&errortext` shape; a live read — a stored cell would be released by the refusal's own `Fail` |

### Generators (multi-produce requests; per-site queue, REPLACE-on-refire)

| builtin | produces | notes |
|---|---|---|
| `lens(uri)` | each projected root's ref-token | `openLensUri`; dedupe by canonical identity; re-seeds `&lens`; refusal → fails |
| `kids(cell)` | each child's ref-token — `d.contains` head then its `d.seq` run | snapshot resolved at request time (the `!x`/`childrenAll` hole, I3) |
| `lroots(cell)` | the session's *current* roots' ref-tokens | `roots_` recomputes per `refresh()` — the post-refresh root list `&lens` alone can't give |

### Gestures (single-answer requests; `Err` → fail)

| builtin | produces | notes |
|---|---|---|
| `step(cell, dim)` | the posward-linked cell's ref-token | **single-produce**, not a generator; `dim` resolved/minted at compile time (`findOrMintDimension`, `vortex_core.hpp:181`) — a gate-time dim mint would land above live marks; negward deferred (§6.7) |
| `lmount(c)` | the delegate root cell's ref-token | `sessionMountAt`; idempotent re-mount |
| `lunmount(c)` | the mount-point cell's ref-token | boundary pre-resolved (VQL Q6 row) |
| `lnext(c)` | the materialized stub's ref-token; fails at exhaustion | `sessionNextPage`, mount-scoped — plain `WindowedLens` sessions scope to themselves |
| `lstub(c)` | the pending stub's ref-token; fails when none | `nextStubAt` — const, not dirty-gated |
| `lcommit(c)` | applied-count scalar | `sessionCommitAll`; `c` is a session handle; **non-atomic** — a refusal can leave ≥0 applied (D4) |
| `ledit(c, "text")` | the cell's ref-token | `sessionEdit`; fails on ref-bound/unkeyed/non-writable/epoch-stale |
| `lrevert(c)` | the cell's ref-token | `sessionRevert`; session handle; fails on source-vanished |
| `lbrowse(c, "t")` | the new query's root cell's ref-token | `sessionBrowseAt` — sql-only, mount-scoped; needs the Q6 `isBrowsed` gate or re-fire duplicates |
| `ldirty(c)` | `dirtyCount` scalar | const read; session handle |
| `lrefresh(c)` | the session's first root's ref-token, post-refresh | `refreshIfChanged` — **mints and writes**, dirty-gate-exempt; session handle |
| `lclose(c)` | — | releases the session — deferred (`U-V2`, as the siblings) |

### Display and pure probes

| builtin | produces | notes |
|---|---|---|
| `ltext(c)` | the cell's rendered content as a string | **the readable-print verb** — `write(c)` prints the ref-int |
| `lshow(c)` | a one-line `"key — content"` summary | debug display; falls back to `ltext` on unkeyed cells |
| `lkey(c)` | the `ExternalKey` string | **fails** on attr/authored cells — failure is the honest absence, strictly better than VQL's `""`-collision |
| `lcell("fs:…")` | the key's current ref-token; fails when unprojected | `cellFor` via the registry scan — the **epoch-surviving** handle: re-resolution across D5 truncation and dead refs (refs themselves survive `start()` only while no drain truncates them — one arena) |
| `lscalar(c)` | `Int64`/`Double`/`Bool` scalar | `valueBits` of lens-minted kinds (`mintNodeCell`); **kind preserved** — `makeScalarCell(bool)`/`(double)`, never `int64` (VPL's `mintRank` kind loss); fails on span/handle kinds |
| `leq(a, b)` | `b` | **validated** ref-identity: `=` on two live ref-ints already IS identity; `leq` additionally resolves both sides through `contains()`+epoch — the `leq(&lens, &null)` hygiene (`toInt("")=0=noCell`) and the dead-handle detector |

**Reserved names**: `write`, `lens`, `kids`, `lroots`, `step`, `lmount`, `lunmount`, `lnext`,
`lstub`, `lcommit`, `ledit`, `lrevert`, `lbrowse`, `ldirty`, `lrefresh`, `lclose`, `lkey`, `lcell`,
`ltext`, `lshow`, `lscalar`, `leq` — builtin dispatch precedes `procTargets_`
(`compiler.cpp:696,:724-728`), so a procedure declaration with one of these names is a
`CompileError`, not a shadow.

## 5. Worked example

```vicon
# open a dir lens; produces each root's ref-token in turn; &lens re-reads the
# first live root at produce time
r := lens("dir:/tmp/lensj")

# the listing — kids() produces ref-ints, so print text, not the token
every c := kids(r) do write(ltext(c))

# mount git under the repo cell — lkey fails on attr cells, so the if prunes
every c := kids(r) do
  if lkey(c) = "fs:52:794498" then {
    g := lmount(c)
    every k := kids(g) do write(ltext(k))
    lunmount(c)
  }

# a sql: lens — one root (the conn cell), schema-only until browsed
s := lens("sql:/tmp/lensj/data.db")
q := lbrowse(s, "t")             # the browsed query's root cell
pg := step(q, "d.contains")      # page 0 — step lands on the page, not a row
sight := step(pg, "d.contains")  # first sight (sql.sight)
row := step(sight, "d.subject")  # the subject row cell
val := step(row, "d.contains")   # first <row>:v:<col> value cell — the editable
ledit(val, "99")
write(ltext(val))   # "99"
write(ldirty(s))    # 1 — a count scalar, not a ref
write(lcommit(s))   # 1 — applied count; a refusal can still leave >=0 applied
ledit(val, "x")
lrevert(val)        # drop the pending edit; restate reads the source back

# pages, keys, identity — statements separate by newline or ;
lstub(s) | write("no stub")
write(lkey(row))
leq(lcell(lkey(row)), row)      # lcell re-resolves key->ref; = on the tokens is
                              # the same test — leq adds the dead-handle check
```

`lens("…")` at statement level produces each root's ref-token; `r` rebinds per produce — it holds the **last** root (correct for single-root dir:/sql:; a union forest's program re-runs per root).
Inside `every`, `c := kids(r)` re-binds `c` per produce — the `Bind` runs inline before the
produce lands the body. `write(c)` would print the dense index — `ltext` is the readable-print
verb. `(lmount(c) | write(&lenserr))` is the refusal-recovery idiom, and it reads the *actual*
reason because `&lenserr` is a produce-time read, not a trailed cell; `lbrowse` failing on a
non-sql session is how `if lbrowse(…) then …` guards compose. The `sql:` walk is the lens's real
shape — query `+d.contains` page `+d.contains` sight `+d.subject` row `+d.contains` `:v:` cell —
and `ledit` refuses anything but a materialized, browsed `:v:` cell (`sql_lens.cpp:1034-1038`).
Requests never reach the result stream — `every … do write(ltext(c))` prints only text.

## 6. Open questions

1. **`lnext` idempotence on re-drive** — a per-site activation token could make `NEXT` re-fires
   replay rather than consume; Icon's consume semantics are the default — deferred.
2. **`lclose`** — releases a session (`U-V2`, shared with the siblings).
3. **`meta:`** — the Driver is store-less; refused like VQL/vlace until a `MetaSource` fold exists.
4. **Non-Driver hosts** — anything that runs a compiled program's cursor through `vm_.run` and is
   not the Driver treats `lensGateOp` as an inert `Halt`, i.e. **end-of-run mid-gesture**:
   `goalResults`/`goalBoundedFirst` (`vortex_stdlib.cpp:3778-3829`), `runPipeline`'s param
   pipelines (`vortex_vm.cpp:1031-1034,:1081-1086`), `stepScheduler` (`:1160-1203`), and any future
   `VortexHost`-embedded run (VQL's Q5 `--engine vortex` class). `viconc` compiles only, so it is
   unaffected.
5. **Co-expressions** (`create`/`@`, spec §10, optional) — a suspended cursor resuming inside
   another activation issues requests through the same gate; interleaving per-site queues is
   unstudied.
6. **`lerr()` builtin spelling** — `&lenserr` chosen (the `&errortext` shape); both parse free.
7. **Negward `step`** and an `lboundary` mount-climb — deferred like vlace's `linked`.
8. **A tagged cell currency** — the Int64 ref-token's collision space (`kids(42)`, a `lscalar`
   `42` vs cell 42) is inherent; a `CellRef` alternative in `CellValue` (or a tagged wrapper
   scalar) would close it at the cost of every `toInt` consumer — deferred; the `contains`+epoch+
   `sessionFor` validation is the mitigation that ships.

## 7. Proposal Register

| Q | files | change | why not caller/vicon |
|---|---|---|---|
| V1 | `vicon/compiler.{hpp,cpp}` | `CompiledProgram` gains `lensGateOp` + the request record cells (`__vicon_lens_{op,a1,a2,a3,site,cont,out,ok}` — the reason rides Driver state, D3, so no `err` cell); `emitCall` dispatch for `lens`/`kids`/`lroots`/`step`/`lmount`/`lunmount`/`lnext`/`lstub`/`lcommit`/`ledit`/`lrevert`/`lbrowse`/`ldirty`/`lrefresh`/`lclose`/`lkey`/`lcell`/`ltext`/`lshow`/`lscalar`/`leq` (builtin names before `procTargets_` — the `write` precedence — and **reserved** for procedure declarations); `EKeyword` arm adds `&lens`/`&lenserr` emitting produce-time-read ops; per-site id minting for the generators; request re-send `altOp`s routed through `guardedAlt`; `step`'s dim resolved/minted at compile time; arity ≤3 errors → `CompileError` | The emission seam lives only here — **zero lexer/parser/token changes**: calls are `ECall`, keywords are `EKeyword`. |
| V2 | `vicon/driver.{hpp,cpp}` | the gate loop inside `resume()` — **through V3's `goalStep` only** (`goalProduce`/`goalResume` treat a non-`haltOp` stop as failure and unwind the activation, `vortex_stdlib.cpp:3752,:3772`); per-site answer queues with **REPLACE-on-refire** semantics and `contains()`+epoch-validated pops; arg decoding `toInt → contains → epoch → sessionFor`; answers written into compile-minted `out`, never gate-time mints; Driver-held `lastLensSession_` (`weak_ptr`) + `lastLensErr_` feeding `&lens`/`&lenserr`; `start()` drains the choice stack (V9), resets `opcodeMap_`, clears queues, re-seeds `&lens`; `sessionFor` over `core().lensSessions()`; canonical-URI open dedupe | The Driver alone owns the live cursor — it is the host half of the protocol. |
| V3 | `vortex_stdlib.{hpp,cpp}` | `goalStep` — the multi-halt variant returning Produced/GatePending/Exhausted — is the **only** resume path a gate program may take; `goalProduce`/`goalResume`/`goalResults`/`goalBoundedFirst` all stop or unwind at `lensGateOp` | std:goal owns the suspension protocol. |
| V4 | `vortex_core.{hpp,cpp}` + `lens_repl.{hpp,cpp}` + `projection_session.{hpp,cpp}` + `arena_manifold.{hpp,cpp}` | **the VQL Q4 row, shared** (`lensSessions()` registry, canonical source-identity recorded at open — normalize `dir:/x`/`/x`/`./x` to one session; `LensRepl::session_` `unique_ptr`→`shared_ptr`; arena-scoped `sessionFor`) **plus the D5 repair**: `ArenaManifold::releaseEpoch()` bumped per `release()`; session `keyToCell_`/`cellToKey_`/`roots_`/`presentKeys_` stamped and wholesale-invalidated on epoch mismatch; `cellToKey_` insert-or-assign on re-mint; **and the D4 repair**: `dirty_` becomes `unordered_map<ExternalKey,string>` key→pending-text — `commit()` applies `dirty_[key]`, never `arena.textOf(*cell)` | One registry serves every frontend; the epoch + pending-text repairs are session/arena-level though vicon-motivated. |
| V5 | `session_ops.{hpp,cpp}` + `sql_lens.{hpp,cpp}` | **the VQL Q6 row, shared — still proposals**: `sessionBrowseAt` (gated on `isBrowsed` — `browse()` mints unconditionally today, `sql_lens.cpp:442-466`), `sessionMountScopeAt`, the `SqlLens` root-key accessor (or browse-returns-key), `sessionUnmountAt` boundary pre-resolve; new `session_ops.cpp` keeps `SqlLens` out of the header | The resolvers all frontends need. |
| V6 | `arena_manifold.{hpp,cpp}` + `projection_session.{hpp,cpp}` + `vortex_core.{hpp,cpp}` | **the VQL Q7B row, shared — moved into the arena**: the `sessionFor` guard inside `ArenaManifold::link`/`linkChain`/`setValueBits` (the real boundary — `VortexCore::link` is a thin wrapper, `vortex_core.cpp:128-167`; `VortexVM`'s `New`/`Clone`/`Call`/`Return`/`MakeVar`/`MakeTerm`/`Unify`/`spawnCursor`/`setCursorOpcode` and `cloneGenerator` write the arena directly); session-internal writes exempt via a `ProjectionSession`-set inside-session-write flag | The funnel no caller can bypass; vicon adds no write verb. |
| V7 | `apps/vicon/main.cpp` + `apps/viconc/main.cpp` | no `:lens` verbs — the acceptance journey is a *program*; document `lensGateOp` as an inert `Halt` for every non-Driver `vm_.run` consumer (§6.4) | The language is the surface; the runner gains nothing. |
| V8 | `tests/xuzz/vicon_lens_test.cpp` (new) | the §1 journey: `lens` → `kids` → `lmount` → back → `ledit`/`lcommit`/`lrevert`/`lnext`/`lstub`/`lbrowse` with ref-tokens (`write(ltext(c))` prints text, `write(c)` prints ints); refusal-as-failure (`(lens("meta:x") \| "refused")` produces `"refused"`, `&lenserr` reads the *actual* reason); re-fire honesty (`{1 to 2; ledit(…)}` edits twice — `dirty_` carries the text); epoch invalidation (a backtrack truncating session mints does not alias); `&lens` re-reads roots post-refresh; reserved-name compile errors | A test sits with its code — the acceptance artifact. |
| V9 | `vortex/vortex_vm.{hpp,cpp}` | the `start()`-drain primitive: release every outstanding mark in stack order (the foreign-cursor pop in `backtrack` drops the frame *without* touching its mark, `vortex_vm.cpp:198-216`, so `outstandingMarks_` never returns to 0 — `compact()` refused, `arena_manifold.cpp:1699-1701`); `opcodeMap_` reset or epoch-keyed so stale kinds cannot alias after index reuse | The VM owns marks and opcode kinds. |

The session machinery (`session_ops`, `ownsCell`, `canApply`, `revertKey`, `commit()`→
`expected<int>`, `windowedFor`/`nextStubAt`, the `openLensUri` scheme guard) is the P5–P8 layer —
vicon's requests call it; the proposed rows are named as VQL's Q4/Q6/Q7B, not existing code.

## 8. What this is not

- Not new opcodes — the spec's §8.1 "no ninth opcode" stands; the gate is a second `Halt`, not an
  instruction.
- Not a typed cell value — produces are Int64 ref-tokens, validated `contains`+epoch+`sessionFor`
  at every boundary; `CellValue` gains no `CellRef` alternative (§6.8 defers the question).
- Not an `IO` value type — requests are suspensions the Driver performs, not values a program
  inspects (contrast vlace's `runIO` tags).
- Not a session stack — "back" is a bound variable.
- Not REPL verbs — `vicon` gains no `:lens` command; the language is the surface.
- Not a re-evaluation firewall — re-fire is Icon's contract; the idempotence lives in *session
  truth* (`dirty_`'s pending text, `isMounted`, the browse gate), not in where a call may sit.
- Not undoable gestures — the arena image of a session write *does* backtrack (the trail replays
  it away); the source and session truth do not. The payload lives where backtracking can't reach.
- Not yet xuzz's picker surface — different frontend's doc.
