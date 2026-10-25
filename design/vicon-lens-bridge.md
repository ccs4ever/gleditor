# vicon Lens Bridge: Lenses as Suspended Host Requests in a Goal-Directed Language (v1.0)

A design specification for how a **vicon program** — not a `vicon` REPL verb — opens, traverses,
mounts, writes back, and releases projection lenses. Design deliverable only: the signatures are
proposals; the `write` builtin's produce-and-suspend protocol and the `session_ops` gesture layer
are the seam, not a new layer. The sibling specs are
[`vql-lens-bridge.md`](vql-lens-bridge.md) v6.6, [`vpl-lens-bridge.md`](vpl-lens-bridge.md) v1.0,
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
| `write(e)` | the sole host-effect builtin — `emitCall` special-cases it: tee the produce to `resultCell_`/`haltOp_`, then forward to the consumer on resume (`compiler.cpp:696-722`) |
| `Driver::resume()` | the suspension protocol: `goalProduce` spawns the cursor under the sentinel choice point; `goalResume` lands it on `failOp_`; both require `pc == haltOp` (`driver.cpp:32-52`, `vortex_stdlib.cpp:3738-3776`) |
| `every` / `start` / `runAll` | the Driver API (`driver.hpp:38-67`); **one `Driver` per process** (`vicon/main.cpp:121`) owns `arena_`, `core_`, `compiler_` (`driver.cpp:15`) |
| `x := e` | `Bind`/`Resolve` on the cursor's `+d.vars`/`+d.values` scope (`compiler.hpp:19-21`; `vortex_vm.cpp:705-725`) |
| `&name` | keyword primary — lexed generically (`lexer.cpp:244-252,:275-277`), emitted for `&null`/`&fail` only (`compiler.cpp:194-202`); `&lens` needs no lexer or parser work |
| `f(args)` | `ECall` — unknown names are `CompileError::UnboundIdentifier` (`compiler.cpp:724-728`); builtins dispatch *before* `procTargets_` (`write`'s arm precedes the lookup) |

vicon has **no lens surface at all** — no `:lens` REPL verbs, no `LensRepl`, no session registry, no
traversal verb (`!x` and `s[i]` are parsed but unimplemented, `compiler.cpp:203-211,:271-275`). The
bridge introduces every piece.

## 3. Design decisions

### D1 — Gestures are suspended *requests* on a second `Halt`; the Driver performs them

The vicon spec's §8.1 argument — "no ninth opcode, no new `ChoicePoint` field" — holds: the
`OpcodeKind` table (`vortex_vm.hpp:27-87`) gains no `Lens*` entries. Instead a lens builtin emits a
**request fragment**, generalizing the `write` tee: `Value` ops write the request record
(`__vicon_lens_op` = gesture id, `__vicon_lens_a1`/`_a2` = arg cells, `__vicon_lens_cont` = `opref`
of the site's continuation), then `Jump lensGateOp` — a second `Halt` minted per program alongside
`haltOp_`/`failOp_` (`compiler.cpp:782-783`). `vm_.run` stops on any `Halt`; the Driver's
`resume()` loop distinguishes: `pc == haltOp_` → deliver `resultCell_`; `pc == lensGateOp` → decode
the record, run the `session_ops` call **host-side**, write `__vicon_lens_out` (answer cell) and
`__vicon_lens_ok` (0/1), `setCursorOpcode(cont)`, and run again — all inside one `resume()`, so a
request never surfaces as a program result (`driver.every`'s sink sees only real produces,
`driver.cpp:54-58`). The site's continuation is `Branch ok → produce out`, else `Fail`.

On `expected`-refusal the Driver writes `ok := 0` and `__vicon_lens_err := reason` — the call
**fails** (D3). The request cells are compile-minted, so Driver writes to them are trailed like any
other write and backtrack cleanly; the session effect they describe persists (D4).

`lens(uri)` and `kids(c)` produce **several** results: the Driver holds a per-call-site answer
queue (`unordered_map<site-id, deque<CellRef>>` — the request carries a compile-minted site
scalar); the fragment's `altOp` re-sends `NEXT site`, the queue empties, the next `NEXT` writes
`ok := 0` → Icon failure. One host round-trip per produce, lazy by construction.

### D2 — Sessions live on `VortexCore`; `&lens` is the `$lens` equivalent

The Driver is process-lived, so a core-held registry survives `start()` replacing the program — the
**same `lensSessions()` row VQL's Q4 registers** (arena-level `weak_ptr` membership + owning
`vector<shared_ptr>`), scanned by `sessionFor`. **Open dedupe** by canonical source identity (the
inherited constraint — normalized URI recorded at open on *every* path): `lens("dir:/x")` under a
live session re-binds the existing roots; two sessions over one writable source would split
`dirty_` and cross-clobber on `commit()` (`keyToCell_` is per-session,
`projection_session.hpp:200-201`).

`&lens` produces the most recent *live* open's `roots().front()` — written into the program's
`lensRootCell_` on each successful open request and re-seeded at `start()` from the registry
(expired `weak_ptr`s skipped, so a closed session leaves `&lens` producing `&null`, not a stale
root). It is the `&pos`/`&subject` pattern: driver-maintained ambient state a keyword produces —
the `+d.scan` cursor precedent (`vortex_stdlib.hpp:383-402`) in miniature.

### D3 — Refusal is Icon failure; `&lenserr` carries the reason

Every `session_ops` verb returns `std::expected`; the gate maps `Err` to *failure*, not an error
value — `lmount(c)` inside `every c := kids(r)` simply prunes that cell; `(lmount(c) |
write(&lenserr))` recovers; `if lmount(c) then …` guards. This is vlace's `Result String`
translated into goal-directed idiom, and it is strictly more honest than VQL's refusal-is-empty
relation: a refused gesture cannot be mistaken for an empty walk. `&lenserr` produces the last
recorded reason cell (`&null` when clean) — Icon's `&errortext` shape.

### D4 — Re-firing is Icon's effect contract; the ops are idempotent or failing under it

VQL needed a static positional rule because its relation re-evaluates *silently* (a `where`/join
re-runs the step). vicon's rule is simpler and honest: **a request fires each time control passes
through its call site, and no session effect is undone on backtracking** — Icon's own contract for
`write`/`open`/`seek`. Every re-fire has a written generator behind it — a loop back-edge, an
alternation arm, a re-driven sequence element (`{1 to 3; ledit(c,"x")}` edits three times), a
re-called procedure — there is no predicate/join/`return` hidden multiplicity. Auditing the gesture
set under re-fire:

| op | second fire |
|---|---|
| `lens` | re-binds via open dedupe — benign |
| `lmount` | **idempotent** — `isMounted` short-circuits before the dirty-gate (`session_ops.hpp:62-66`) |
| `lunmount` | no boundary → `Err` → fails — a re-driven path simply prunes |
| `ledit` | same text re-applies — idempotent |
| `lcommit` | applies 0 — benign |
| `lrevert` | non-dirty is a true no-op (`session_ops.hpp:165-170`) |
| `lbrowse` | `isBrowsed` → `Err` → fails (`sql_lens.hpp:121-122`) |
| `lnext` | materializes one more page — consumption, the one honest cost of re-drive (named, not guarded) |
| `lstub`/`ldirty`/probes | const — safe |

So **no positional ban**: there is no position a static rule could call safe (a shared continuation
downstream of any `#Choice` re-fires), and the ops that could corrupt are already idempotent or
self-refusing. Two named residues: `lnext` over-consumes on re-drive (the `every`-driven
`lnext(c) \ n` shape is the intended paging idiom); and the broken `e \ n` kill-flag
(`compiler.cpp:14-22` — trailed flag writes cannot persist) means a gesture inside `\n` fires past
its bound — the same deviation I2 already carries for `write`.

### D5 — Session mints live above the VM's marks: the truncation hazard and its repair

vicon is the only frontend whose gestures run **while cursor choice-point marks are outstanding**.
A mount/browse/`refresh()` mints session cells above the enclosing marks' floors; a later backtrack
that releases one of those marks truncates `slots_` to `m.cellCount`
(`arena_manifold.cpp:1667`) — the session's `keyToCell_`/`cellToKey_`/`roots_` now name dead
`CellRef`s. `refresh()`'s own internal mark is the innermost and nests legally
(`projection_session.hpp:96-97`); the hazard is the *outer* release.

Rule: **identity-map reads lazily self-heal** — `keyFor`/`cellFor`/`ownsCell`/roots iteration
evict any ref failing `arena().contains`; the next `refresh()` re-mints the key (same key → same
`CellRef` is already the mint rule, `projection_session.hpp:82-84`), and the Driver validates
`contains` on answer-queue pops. A truncated projection therefore degrades to "unprojected" — the
program's next `lrefresh`/`lens` re-sees it — rather than dangling. This is the vicon-era analogue
of vlace D4's "sessions die with the Compiler": sessions outlive their mints' marks. (The
compile-minted flag question it raises is already confused in-tree — `compiler.cpp:14-22` says
writes to compile-time cells *are* trailed while `emitIf` at `:568-574` says they are not;
`arena_manifold.hpp:711-720`'s older-than-the-mark rule settles it: they are trailed, and the
if-guard only survives because `init` re-runs per activation.)

### D6 — `session_ops` carries the gesture protocol; `union:` is key-level; `meta:` refused

The builtins call `sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/
`sessionEdit`/`sessionRevert`/`sessionDirty` (`session_ops.hpp:42-171`), inheriting the dirty-gate
(edit/commit/revert exempt), `canApply` inside `editContent`, and `commit()`→`expected<int>`.
Cell→session resolution is the `ownsCell` scan (node O(1), attr one negward hop,
`projection_session.hpp:49-60`); key-needing ops resolve `keyFor` and fail unkeyed. `lnext`/
`lstub`/`lbrowse`/`lunmount` route mount-scoped — cell → `keyFor` → `mountBoundary`
(`router_lens.hpp:70-71`) → `windowedFor`/`nextStubAt` — through the **shared `session_ops.cpp`
rows VQL's Q6 registers** (`sessionMountScopeAt`, `sessionBrowseAt`, the `SqlLens` root-key
accessor); `mountBoundary`→`nullopt` fails the call (a plain `WindowedLens` session —
`sql:`/`log:`/`journal:`/`git:` opened directly — scopes to itself). `union:` supports key-level
ops only (`UnionLens` is neither `RouterLens`/`WindowedLens`/`SqlLens`, `union_lens.hpp:31`) — the
scoped verbs fail, and the duplicate-`ExternalKey` refusal is the shared `claimedBy` probe
(first-success write vs last-emission display, `union_lens.cpp:78-92`). `meta:` stays refused in
`openLensUri` (`lens_uri.cpp:33-50`) — the Driver is store-less like vlace's evaluator; no
`MetaSource` fold exists to borrow.

### D7 — vicon's write surface is scope-wiring; the shared funnel already covers the rest

A vicon program has **no link verb** — the only compiled writes on user data are `Link`/`Value`/
`Splice`/`Break`/`Clone` opcodes routing through `core_.link`/`core_.value`/`core_.breakLink`
(`vortex_vm.cpp:526-640`), which is exactly the `VortexCore` write-method funnel VQL's Q7B
registers; `Bind`'s scope wiring writes `arena().link` directly (`vortex_vm.cpp:715-725`) and
**must** stay outside the funnel — `r := lens("dir:/x")` links the session cell negward on
`d.values`, a write the trail undoes on backtrack (the session cell is older than the mark →
trailed → slot restored, `arena_manifold.cpp:1649-1656`), so binding projected cells is both
necessary and self-cleaning. `ledit` is the content-write path. When `!x`, subscript, or
`zzLink`-class verbs land (I3+), the funnel already governs the opcodes they emit.

## 4. Proposed surface

### Keywords

| keyword | produces | notes |
|---|---|---|
| `&lens` | the most recent live session's first root; `&null` when none | driver-maintained; re-seeded at `start()` |
| `&lenserr` | the last refusal's reason; `&null` when clean | `&errortext` shape |

### Generators (multi-produce requests)

| builtin | produces | notes |
|---|---|---|
| `lens(uri)` | each projected root in turn | `openLensUri`; dedupe by canonical identity; writes `&lens`; refusal → fails |
| `kids(cell)` | each child — `d.contains` head then its `d.seq` run | snapshot resolved at request time (the `!x`/`childrenAll` hole, I3) |
| `step(cell, dim)` | the posward-linked cell | general traversal; negward deferred |

### Gestures (single-answer requests; `Err` → fail)

| builtin | produces | notes |
|---|---|---|
| `lmount(c)` | the delegate root cell | `sessionMountAt`; idempotent re-mount |
| `lunmount(c)` | the mount-point cell | boundary pre-resolved (VQL Q6 row) |
| `lnext(c)` | the materialized stub cell; fails at exhaustion | `sessionNextPage`, mount-scoped |
| `lstub(c)` | the pending stub cell; fails when none | `nextStubAt` — const, not dirty-gated |
| `lcommit(c)` | applied-count scalar cell | `sessionCommitAll`, once per call |
| `ledit(c, "text")` | the cell | `sessionEdit`; fails on ref-bound/unkeyed/non-writable |
| `lrevert(c)` | the cell | `sessionRevert`; fails on source-vanished |
| `lbrowse(c, "t")` | the new query's root cell | `sessionBrowseAt` — sql-only, mount-scoped |
| `ldirty(c)` | `dirtyCount` scalar cell | const |
| `lrefresh(c)` | the session's first root, post-refresh | `refreshIfChanged` — const probe, exempt |
| `lclose(c)` | — | releases the session — deferred (`U-V2`, as the siblings) |

### Pure probes

| builtin | produces | notes |
|---|---|---|
| `lkey(c)` | the `ExternalKey` string | **fails** on attr/authored cells — failure is the honest absence, no `""`-collision |
| `lcell("fs:…")` | the cell | `cellFor` via the registry scan — the stable cross-`start()` handle |
| `lscalar(c)` | `Int64`/`Double`/`Bool` scalar cell | `valueBits` of lens-minted kinds (`mintNodeCell`); fails on span/handle kinds |
| `leq(a, b)` | `b` | `CellRef` identity — `=` compares *rendered* values (the same wrong-answer hole VQL names); comparison idiom: produce right operand or fail |

## 5. Worked example

```vicon
# open a dir lens; produces each root in turn; &lens binds the first
r := lens("dir:/tmp/lensj")

# the listing — kids() is a generator; c rebinds per produce
every c := kids(r) do write(c)

# mount git under the repo cell — lkey fails on attr cells, so the if prunes
every c := kids(r) do
  if lkey(c) = "fs:52:794498" then {
    g := lmount(c)
    every k := kids(g) do write(k)
    lunmount(c)
  }

# a sql: lens — schema-only until browsed
s := lens("sql:/tmp/lensj/data.db")
q := lbrowse(step(s, "d.contains"), "t")
row := step(q, "d.contains")
ledit(row, "99")
write(ldirty(s))     # 1
write(lcommit(s))    # 1
ledit(row, "x")
lrevert(row)         # drop the pending edit; restate reads the source back

# pages, keys, identity — statements separate by newline or ;
lstub(s) | write("no stub")
write(lkey(row))
leq(lcell(lkey(row)), row)
```

`lens("…")` at statement level produces each root; binding `r` keeps the first. Inside `every`,
`c := kids(r)` re-binds `c` per produce — the `Bind` runs inline before the produce lands the body.
`(lmount(c) | write(&lenserr))` is the refusal-recovery idiom; `lbrowse` failing on a non-sql
session is how `if lbrowse(…) then …` guards compose. Requests never reach the result stream —
`every … do write(c)` prints only cells.

## 6. Open questions

1. **`lnext` idempotence on re-drive** — a per-site activation token could make `NEXT` re-fires
   replay rather than consume; Icon's consume semantics are the default — deferred.
2. **`lclose`** — releases a session (`U-V2`, shared with the siblings).
3. **`meta:`** — the Driver is store-less; refused like VQL/vlace until a `MetaSource` fold exists.
4. **A non-Driver host running a compiled program** stops at `lensGateOp` as an inert `Halt` — the
   program ends silently mid-gesture (`viconc` compiles only, so it is unaffected; any future
   `VortexHost`-embedded run is the case to guard — the same class as VQL's Q5 `--engine vortex`).
5. **Co-expressions** (`create`/`@`, spec §10, optional) — a suspended cursor resuming inside
   another activation issues requests through the same gate; interleaving per-site queues is
   unstudied.
6. **`lerr()` builtin spelling** — `&lenserr` chosen (the `&errortext` shape); both parse free.
7. **Negward `step`** — deferred like vlace's `linked`; a `lensBoundary` mount-climb likewise.

## 7. Proposal Register

| Q | files | change | why not caller/vicon |
|---|---|---|---|
| V1 | `vicon/compiler.{hpp,cpp}` | `CompiledProgram` gains `lensGateOp` + the request record cells (`__vicon_lens_{op,a1,a2,site,cont,out,ok,err}`) + `lensRootCell_`/`lensErrCell_`; `emitCall` dispatch for `lens`/`kids`/`step`/`lmount`/`lunmount`/`lnext`/`lstub`/`lcommit`/`ledit`/`lrevert`/`lbrowse`/`ldirty`/`lrefresh`/`lclose`/`lkey`/`lcell`/`lscalar`/`leq` (builtin names before `procTargets_` — the `write` precedence; arity errors → `CompileError`); `EKeyword` arm adds `&lens`/`&lenserr`; per-site id minting for the enqueueing generators | The emission seam lives only here — **zero lexer/parser/token changes**: calls are `ECall`, keywords are `EKeyword`. |
| V2 | `vicon/driver.{hpp,cpp}` | the gate loop inside `resume()`: `run` → `pc == lensGateOp` → decode record → `session_ops` → write `out`/`ok`/`err` → `setCursorOpcode(cont)` → `run` again; per-site answer queues with `arena().contains`-validated pops; `&lens` write on open + `start()` re-seed; `sessionFor` over `core().lensSessions()`; canonical-URI open dedupe | The Driver alone owns the live cursor — it is the host half of the protocol. |
| V3 | `vortex_stdlib.{hpp,cpp}` | `goalProduce`/`goalResume` require `pc == haltOp` (`vortex_stdlib.cpp:3752,:3772`) — a gate-aware variant (multi-halt `goalStep` returning Produced/GatePending/Exhausted) so V2's loop does not hand-roll `vm_.run`/`setCursorOpcode` | std:goal owns the suspension protocol. |
| V4 | `vortex_core.{hpp,cpp}` + `lens_repl.{hpp,cpp}` + `projection_session.{hpp,cpp}` | **the VQL Q4 row, shared**: `lensSessions()` registry (`weak_ptr` membership + owning `vector<shared_ptr>`), canonical source-identity recorded at open, `LensRepl::session_` `unique_ptr`→`shared_ptr`, arena-scoped `sessionFor`; **plus the D5 repair**: `keyFor`/`cellFor`/`ownsCell`/roots iteration evict refs failing `arena().contains` — vicon is the only frontend that can mint session cells under live marks today | One registry serves every frontend; the repair is session-level though vicon-motivated. |
| V5 | `session_ops.{hpp,cpp}` + `sql_lens.{hpp,cpp}` | **the VQL Q6 row, shared**: `sessionBrowseAt`, `sessionMountScopeAt`, the `SqlLens` root-key accessor (or browse-returns-key), `sessionUnmountAt` boundary pre-resolve, `session_ops.cpp` keeps `SqlLens` out of the header | The resolvers all frontends need. |
| V6 | `vortex_core.{hpp,cpp}` | **the VQL Q7B funnel, shared**: the `sessionFor` check inside `link`/`value`/`breakLink`/`cloneGenerator` — vicon's content-write opcodes already route there (`vortex_vm.cpp:526-640`); `Bind`'s scope wiring stays arena-direct by design (D7) | vicon adds no write verb; the funnel covers the opcode layer. |
| V7 | `vicon/main.cpp` + `viconc/main.cpp` | no `:lens` verbs — the acceptance journey is a *program*; document `lensGateOp` as an inert `Halt` for non-Driver hosts | The language is the surface; the runner gains nothing. |
| V8 | `tests/xuzz/vicon_lens_test.cpp` (new) | the §1 journey: `lens` → `kids` → `lmount` → back → `ledit`/`lcommit`/`lrevert`/`lnext`/`lstub`/`lbrowse`; refusal-as-failure (`(lens("meta:x") | "refused")` produces `"refused"`); re-fire honesty (`{1 to 2; ledit(…)}` edits twice); `&lens` re-bind across `start()` | A test sits with its code — the acceptance artifact. |

The session machinery (`session_ops`, `ownsCell`, `canApply`, `revertKey`, `commit()`→
`expected<int>`, `windowedFor`/`nextStubAt`, the `openLensUri` scheme guard) is the P5–P8 layer —
vicon's requests call it.

## 8. What this is not

- Not new opcodes — the spec's §8.1 "no ninth opcode" stands; the gate is a second `Halt`, not an
  instruction.
- Not an `IO` value type — requests are suspensions the Driver performs, not values a program
  inspects (contrast vlace's `runIO` tags).
- Not a session stack — "back" is a bound variable.
- Not REPL verbs — `vicon` gains no `:lens` command; the language is the surface.
- Not a re-evaluation firewall — re-fire is Icon's contract; the idempotence lives in the ops
  themselves, not in where a call may sit.
- Not yet xuzz's picker surface — different frontend's doc.
