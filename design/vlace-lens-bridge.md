# vlace Lens Bridge: Lenses as First-Class Cells in a Lazy IO Language (v7.3)

A design specification for how a **vlace program** — not the `vlace` REPL — opens, walks, mounts,
writes back, and releases projection lenses. Design deliverable only: nothing here is implemented
beyond what the compiler already does; the signatures are proposals. Rounds 1–7 of
`vortex-design-critique` folded in; dispositions in `design/projects/vlace-lens-bridge-review/`.

An earlier attempt exposed the REPL verb set as string-keyed `IO.Lens*` effects — a session
state machine, the REPL's model, not a lazy functional program's. This redesigns from the
language's idioms outward.

______________________________________________________________________

## 1. Intent

A vlace program should be able to write, in its own types:

- **open a source** as projected root cells — `dir:`/bare path, `git:`, `sql:`, `cfg:`, `log:`,
  `journal:`, `union:` — the `openLensUri` scheme table (`lens_uri.cpp:22-97`);
- **navigate** with the cell/dim natives it already uses — `childrenAll`, `linked`, `cellText`,
  `dimensionNamed`;
- **mount a delegate** under a projected cell (GitLens under a worktree cell, SqlLens under a
  `.db`) and come **back** — "back" falls out of the value model, not a stack;
- **edit, commit, and revert** projected cells on write-capable lenses (`sql:`/`cfg:` today);
- **materialize** a deferred page, and let sessions go out of scope.

Acceptance journey: `open dir → walk listing → mount git under the repo cell → read commits →
release mount → keep walking the dir → edit+commit on a sql: lens → revert a refused edit →
program ends`.

## 2. Current surface (already in the build)

| builtin | type | meaning |
|---|---|---|
| `openLens` | `String -> IO Cell` | `FilesystemLens` only; projects into the evaluator's `VortexCore`; session pinned in `Compiler::sessions_` (`compiler.hpp:223-225`) |
| `cellText` | `Cell -> String` | `textOf` renders only `contentOf` spans — scalar-kind cells (Int64/Double/Bool, SqlLens/ConfigLens typed columns) return `""` — `cellScalar` fills the hole (§4) |
| `linked` | `Cell -> Dim -> Option Cell` | posward step (no negward verb exists) |
| `childrenAll` | `Cell -> List Cell` | `d.contains` head + `d.seq` run (`compiler.cpp:1002-1028`) |
| `dimensionNamed` | `String -> Option Dim` | lookup-or-mint (`ensureDimension`, `compiler.cpp:916`) — always `Some`; the `Option` lies |
| `words` | `String -> List String` | whitespace split |
| `promote` | `Cell -> IO Int` | promotion — the `IO.*`-effect precedent |
| `makeText`/`makeDim`/`setLink` | `String -> Cell` / `String -> Dim` / `Cell -> Dim -> Cell -> Unit` | arena verbs — see D6 |

`==`/`/`/`+`/etc. type `a -> a -> T` but eval refuses non-numeric/non-Str
(`compiler.cpp:195-225`) — a pre-existing soundness hole in the same class as `++` (P2 pins
all of them); there is **no cell-identity predicate** — `cellEq` fills it (§4).

## 3. Design decisions

### D1 — Roots are values; there is no "current lens"

Every `openLens` returns the session's projected roots (a `List Cell` — `union:`/forest sources
emit several; a bare `Result String Cell` would orphan siblings). Sessions coexist; re-opening
the same source mints a *fresh, disjoint* projection (the arena grows unboundedly across
re-opens — priced, D4). "Back" means holding the earlier roots — no stack, no implicit focus.
Price: one `shared_ptr<ProjectionSession>` per open (`unique_ptr` suffices until `lensDrop` —
U-L10 — needs sharing); see D4.

### D2 — Ops take cells, not strings

Programs have the cell; keys are output only. `lensMount` takes the **mount-point cell**;
`lensNext`/`lensStub` take a session-member cell and route it to the right `WindowedLens`
(§4's resolution rule); `lensCommit`/`lensRevert`/`lensEdit`/`cellKey` take a session-member
cell. `cellKey :: Cell -> Option String` is output only — no inverse `keyCell`.

### D3 — A `Cell` is a stable handle into mutable structure; reads are force-time observations

Projected cells are *mutable* — gestures re-enumerate the source into the arena (`refresh()`
restates content and tombstones edges). With lazy memoizing eval, a `childrenAll dir` thunk
forced before `lensMount` caches the pre-mount run; a re-evaluation after sees the mount's
spliced run. The rule: **pure reads of projected cells observe the arena as of when the thunk
is forced** — a `Cell` is a stable name, its content/ranks the current revision. Sequencing
reads vs mutating ops is the program's job via `IO` bind order. `cellKey`/`cellText`/
`childrenAll`/`linked`/`cellEq`/`cellScalar` stay pure-typed with this stated semantics.

Writes-in-pure-reads: `childrenAll` mints `d.contains`/`d.seq` on first use; `dimensionNamed`
mints unknown dims; `numericOp`/`makeText`/`makeDim` mint — arena mutations inside "pure"
reads corrupt mint-accounting for any future `rewind` (foreign-mint refusal,
`projection_session.cpp:415-422`). `setLink` doesn't mint (it links) — its hazard is D6's
provenance rule, not mint-accounting. Noted; matters only if a persistent Compiler ever
shares the arena with a marking runtime.

### D4 — Sessions die with the Compiler, not with cell reachability

`sessions_` is append-only (`compiler.hpp:225`): forgetting a `Cell` releases *nothing* — the
session, its OS resources (db handles, watchers — `dir:`/bare-path lenses hold no fds, they're
snapshot lenses), and identity maps live to evaluation end. The bound today: `Driver` builds a
fresh `Compiler` per `run`/`runIO` (`driver.cpp:50-53`/`92-95`) — sessions die with the *program*.
`lensDrop :: Cell -> IO Unit` is deferred (`U-L10`): needed only if a persistent REPL reuses
one `Compiler`. Latent hazards then: `refresh()`'s `arena.mark()`/discard is not RAII — an
exception in `forEachNode` leaks `outstandingMarks_` (`projection_session.cpp:180-250`); a
dropped session with `checkpoint()` marks leaks them too. Both matter only under U-L10. **`promote` unguarded** on session-owned cells (walks the projected subgraph into real ops on a Store-backed embedder — latent, Driver is store-less); **Ref-aliasing** (U-L5, latent): a `refKeys_`-bound cell shared across sessions makes `ownsCell` first-match-in-`sessions_` arbitrary — unreachable today (`meta:` refused, the only ref-emitter).

### D5 — `openLens` takes URIs, and refusals are `Err` values

`openLens` routes through `openLensUri` (all schemes). Refusals are `std::expected` — so
`IO Cell` can't carry them, and today's `Cell 0` empty-source sentinel
(`compiler.cpp:1206-1215`) is a house-rule violation. `openLens :: String -> IO (Result
String (List Cell))` — `Err` the reason (or "projected no roots" for an empty source), `Ok`
the root list.

The `meta:` hole is fixed in **`openLensUri` itself** (P6 — refuse any *unrecognized*
`scheme:` prefix matching `^[A-Za-z][A-Za-z0-9+.-]*:` — `meta:` included, which today silently
misroutes to `FilesystemLens` at `lens_uri.cpp:80-96`; regression note: `weird:name` files
bearing a scheme-shaped prefix will refuse — documented). A `union:meta:x|dir:/y` recurses
into the same hole — one guard closes both. `union:` stays openable; it's not `WindowedLens`
(`lensNext`/`lensStub` always `Err` on it), `applyContentChange` is first-success/last-error
(`union_lens.cpp:76-88`), and **a `RouterLens` embedded inside a `union:` child is unreachable
for `lensMount`** (session resolution sees the `UnionLens`, not its children — a capability
boundary, noted). `meta:` in vlace needs the arena constructed *over* a store fold — refused
until the evaluator's arena gains a base.

### D6 — Arena verbs on session-owned cells are provenance-breaking

`setLink` writes the arena on *any* `Cell`, including projected ones — bypassing `dirty_`
and the next `refresh()`'s tombstone. Worse, `link()` maintains both ends — `setLink
scratch dim projected` mutates the projected cell's *negward* edge too, and `setLink a d b`
can also displace a *third* cell's reciprocal end (the both-operands guard is complete today
because no verb can create authored↔session adjacency — noted for any future `unlink`/
`insertIntoRank`-style verb). `makeText`/`makeDim` only mint (not provenance offenders —
their hazard is foreign-mint vs `rewind`, D3). Rule: **generic arena verbs must not write
session-owned cells** — the correct write is `lensEdit`, which routes through
`session->editContent`. Guard in the register (P1 — at the `setLink` callsite, using P5's
`ownsCell`) checks *both* `setLink` operands.

## 4. Proposed surface

| name | type | notes |
|---|---|---|
| `openLens` | `String -> IO (Result String (List Cell))` | `openLensUri`; `Err` on refusal/empty roots; `meta:` refused inside `openLensUri` (P6); `Ok` = all roots (union: forests) |
| `lensMount` | `Cell -> IO (Result String Cell)` | `RouterLens::open` under the cell; `Ok` = **delegate root cell**; `Err` = non-RouterLens session, unowned cell, keyless cell, not-a-filesystem-cell (a delegate cell — `fs:` cells beneath a mounted path CAN mount), `delegateFor` refusal, or mount-while-dirty. Idempotent on an already-mounted cell — the idempotence check wins over the dirty-gate (no mutation → no dirty issue). Post-mount `childrenAll` = `[delegate roots…, fs children…]` (reorders). |
| `lensUnmount` | `Cell -> IO (Result String Unit)` | drops the mount boundary at-or-above the cell; `Err` modes as mount + dirty |
| `lensNext` | `Cell -> IO (Result String (Option Cell))` | resolution rule below; `Ok (Some c)` = materialized stub cell, `Ok None` = exhaustion, `Err` = refusal/dirty |
| `lensStub` | `Cell -> IO (Result String (Option Cell))` | resolution rule below; `Ok Some` = pending stub, `Ok None` = none; `Err` = non-windowed — **not dirty-gated** (a const query, exempt) |
| `lensCommit` | `Cell -> IO (Result String Int)` | applies the session's whole `dirty_` set (not just the arg's — the cell selects the session, not the key); `Ok` = dirty keys *attempted* (commit skips vanished cells — `projection_session.cpp:284-287`); `Err` = a refusal with ≥0 already applied (per-cell, non-atomic, `unordered_set` order — `:280-303`); applied count unobservable — `lensDirty` covers it |
| `lensRevert` | `Cell -> IO (Result String Unit)` | `revertKey` (P5): erase the cell's key from `dirty_` + `refresh()` only if it was dirty (non-dirty = true no-op); per-key while `lensCommit` is session-wide — a dropped cell handle is unrevertable yet still committable (asymmetry noted); source-vanished → `Err` ("nothing to restate" — refresh stale-marks the cell but `restateContent` only runs for present keys, so the edit text stays visible; needs a retained `presentKeys_` to distinguish; and a file *replaced* under a mounted path strands `Mount::mountKey` on a stale ghost cell — `mountBoundary` from the replacement returns `nullopt` — broader than the relocating-lens case) |
| `lensEdit` | `Cell -> String -> IO (Result String Unit)` | `session->editContent(key, text)` — which itself probes `lens_->canApply(key)` (P5) so *every* frontend inherits the writability check (xuzz's direct `editContent` path, `xuzz_app.cpp:604`, bypasses `session_ops`); `Err` for ref-bound, unkeyed, or non-writable-lens cells; scalar-kind transitions to text-typed while dirty (kind-restored post-commit). **No refresh after** — see the edit/commit rule |
| `lensDirty` | `Cell -> IO (Result String Int)` | `dirtyCount()` for the resolved session; `Err` on unowned cell |
| `cellKey` | `Cell -> Option String` | pure: `ExternalKey` if a projected **node** cell (`None` for attr/authored cells) |
| `cellEq` | `Cell -> Cell -> Bool` | pure: identity on `CellRef` — `==` on cells returns `TypeMismatch` today (`compiler.cpp:195-225`) |
| `lensBrowse` | `Cell -> String -> IO (Result String Unit)` | `lens->browse(key, table)` — SqlLens only; `Err` on non-sql or already-browsed. A direct `sql:` open is schema-only until browsed (openSqlUri never browses, `sql_lens.cpp:1123-1193`); `dir:`+`lensMount` on a `.db` auto-browses via `delegateFor` |
| `cellScalar` | `Cell -> Option Scalar` | pure: the cell's scalar content — `data Scalar = SInt Int | SFloat Float | SBool Bool` (runtime-registered, P1's reservation covers it); a bare `Option Int` carries Double bits as garbage — `cellText` returns `""` on scalar cells (textOf renders only `contentOf` spans); covers SqlLens/ConfigLens typed columns |

**Windowed resolution rule** (`lensNext`/`lensStub`): the cell's session is a `RouterLens` →
`mountBoundary(keyFor(cell))` → that mount's `WindowedLens` via `windowedFor`/`nextStubAt`
(**P8** — `RouterLens::nextStub()` is first-wins across mounts otherwise); a plain
`WindowedLens` session (`sql:`/`log:`/`journal:`/`git:` opened directly) → itself; neither →
`Err`. `windowedFor`/`nextStubAt` takes a **mount key** (from `mountBoundary`), resolving the
mount's `WindowedLens*` — `mounts_` is private today.

**Edit/commit rule.** `editContent` writes arena text + `dirty_`; `refresh()`'s
`restateContent` reverts dirty cells to source content — `lensEdit` → refresh → `lensCommit`
unprojects the *source*, not the edit. Rules: **`lensEdit` performs no refresh** (edit is
arena-local until commit — xuzz's `editContent` precedent, `xuzz_app.cpp:607`); **mutating
gestures refuse while the resolved session's `dirty_` is non-empty** (per-session; exempt set
explicit: `{lensEdit, lensCommit, lensRevert}` — a second edit must be allowed, and commit/
revert are the only exits); P5 adds a dirty-preserving `restateContent` (skip `dirty_` keys)
**and** makes `commit()` re-insert *all* un-attempted keys on refusal (today only the failing
one — silent edit loss, `:283-293`); `commit()`'s internal `refresh()` runs on success only
(`:304`). The "gesture + `refresh()`" rule scopes to `lensMount`/`lensUnmount`/`lensNext` only.
`lensRevert` is the dirty-state exit — without it a refusing key wedges the session.

**Session resolution** scans `sessions_` calling `s->ownsCell(cell)` (P5's predicate — *not*
a re-specified climb, so `managedDims` stays private) — `cellToKey_` for nodes (O(1)); attr
cells climb **negward on a session-managed dim** (the attr rank — `d.role`/`d.mime`/`d.size`/
`d.mtime`/`d.perm`, not `d.contains` — `writeAttribute` links `node -(rank)-> attr` posward,
`projection_session.cpp:107-121`) to the owning node for *session* discovery, but the op's
key stays the cell's own or `Err`/`None` "no key" (never retargets the node's body — all
key-needing ops). Session-only ops (`lensCommit`/`lensDirty`) proceed on the climbed session.
Authored cells → `Err`/`None` the same way.

`Result`/`Option` shapes are deliberate: every underlying verb returns `std::expected` — a
`String` Err matches `expected<string>` throughout (a `LensError` ADT is a later refinement),
and `RouterLens::open` returns the delegate root key — vlace §6.6: "failure that a program
should handle is a value in its type." `P1` handlers wrap `runIO`/`installXanadu` work in
try/catch-to-`Err` (`intern()`/`mintSlot`/lens `browse` can throw, not refuse).

## 5. Worked example (self-contained — `kPrelude` has no production path)

```vlace
find f xs = case xs of [] => None ; (h : t) => case f h of True => Some h ; False => find f t ;
foldr f z xs = case xs of [] => z ; (h : t) => f h (foldr f z t) ;

openLens "/tmp/lensj" >>= (fn opened =>
  case opened of
    Err why => putStrLn ("open: " ++ why)
  ; Ok roots =>
      case roots of
        [] => putStrLn "empty"
      ; (dir : _) =>
          case find (fn c => cellText c == "repo") (childrenAll dir) of
            None => putStrLn "no repo"
          ; Some repo =>
              lensMount repo >>= (fn mounted =>
                case mounted of
                  Err why2 => putStrLn ("mount: " ++ why2)
                ; Ok git =>
                    putStr (foldr (fn c => fn acc => cellText c ++ acc) ""
                                  (childrenAll git)) >>
                    lensUnmount repo >>= (fn unmounted =>
                      case unmounted of
                        Err _ => putStrLn "unmount failed"
                      ; Ok _ => putStrLn "back to files")))
```

`find`/`foldr` are defined inline (the prelude isn't loaded by `Driver`; `LensVlaceGate` does
the same, `lens_vlace_gate_test.cpp:111`). The `[]` alt comes **first** — a `;`-separated alt
binds to the innermost unparenthesized `case`, so `(dir : _)` attaches to `case roots` while
`case find` stays nested inside it (the naive order puts `[]` last, where `;` binds to
`case find` instead — a real dangling-`;` trap worth one sentence). `dir`/`git`/`repo` are
bound once and reused. `lensUnmount`'s `Result` is `case`d — modelling the failure-is-a-value
idiom the surface is built around.

## 6. Open questions

1. **`lensMountWith`?** Mount by cell auto-delegates (`RouterLens::delegateFor`); forcing a
   specific delegate is deferred.
2. **`meta:`** stays refused (P6): needs the evaluator's arena constructed over a store fold.
3. **`keyCell`** — `cellKey`'s inverse — not proposed; output-only naming.
4. **Negward climb.** `linked` is posward-only; a program holding a delegate cell can't climb
   to its mount cell — D1's retained-root model covers it; `lensBoundary :: Cell -> Option
   Cell` deferred.
5. **`lensDrop`** (`U-L10`) — deferred; needed only if a persistent REPL reuses a `Compiler`.
6. **`applyAttributeChange`** exists in the trait (`projection_lens.hpp:179-183`) but no verb
   or `dirty_` tracking drives it — attr editing is silently impossible; noted, not proposed.
7. **A worked program for edit/commit/revert/stub/dirty/cellKey/cellEq** — §4 gives their
   semantics + the register; a fuller worked example is a documentation follow-up.
8. **`lensSync`/`lensPoll`** — deferred; a clean `lensCommit`/`lensRevert` is the implicit
   sync path today (refresh runs inside commit/revert).

## 7. Proposal Register

| P | files | change | why not caller/vlace |
|---|---|---|---|
| P1 | `compiler.cpp` | `installXanadu`: `IO.LensMount`/`LensUnmount`/`LensNext`/`LensStub`/`LensCommit`/`LensRevert`/`LensEdit`/`LensDirty` ctor tags + `runIO` handlers; `cellKey`/`cellEq`/`cellScalar` pure natives (`makeNative`); `openLens` → `openLensUri` + `List Cell` roots + empty-roots `Err`; `refOfWrapped` forces inner arg **and reserves every tag the runtime interprets** — `Cell`/`Dim`/`IO.*`/`Nil`/`Cons`/`Unit`/`Tuple`/`Ok`/`Err`/`Some`/`None`/`True`/`False`/`SInt`/`SFloat`/`SBool` (ECtor + registerData refuse — forgeable-ctor + `data Cell = …`/`data W = Some` collision holes closed; `Unit` registers arity-0 — `Ok Unit` fails typecheck today while matching at eval); **`tests/xuzz/lens_vlace_gate_test.cpp` updated** — its program does `openLens … >>= fn root => childrenAll root` (a `Result` under the new signature — unwrap `Ok` → destructure `[root]`); **`setLink` guard at callsite** (ownsCell on both operands); `runIO` handlers arity-check (`args.size() == arity → Err` — an under-saturated `IO.*` tag must not `out_of_range`) and wrap in try/catch-to-`Err` | The effect surface lives only here. |
| P2 | `types.cpp` | Schemes for §4; pin `++ :: String -> String -> String` **and** `==`/`/=` → `{Int, Float, String, Char}` (Char evals as Int — `tyChar` would otherwise refuse a comparison eval accepts; `Bool` `==` fails at eval — noted), `<`/`<=`/`>`/`>=` **and** `+`/`-`/`*`/`/`/`%` → `{Int, Float}` (eval ground set — `Str` is legal only for `==`/`/=`); mechanism: reject `prune`→`Con` not in the set, unresolved `Var` allowed (keeps polymorphic helpers; the residual `Cell`-instantiation hole documented); `++`'s pin reconciles vs spec §5.1/§7.4 (`std:fun/append`) — a spec deviation flagged | Normative typecheck needs declared types + the pin rule. |
| P3 | `compiler.hpp` | Session-resolution helper — scan `sessions_` calling `s->ownsCell(cell)` (P5's predicate, *not* a re-specified climb — `managedDims` stays private); shared by ops + `cellKey`/`cellEq`/`cellScalar` | Shared by every op; DRY against open-coded copies. |
| P4 | `prelude.hpp` | `find` only (concatMap dropped — unused) | Prelude wiring into Driver is a separate U-L item; the prelude isn't loaded by any production path today. |
| P5 | `projection_session.{hpp,cpp}` | `ownsCell(CellRef)` (`cellToKey_` for nodes + **exactly one** negward step on a managed dim for attr cells — bounded, not iterative); **dirty-preserving `restateContent`** (skip `dirty_` keys — fixes xuzz's `refreshIfChanged` clobber too, `xuzz_app.cpp:615`); **`commit()` re-inserts ALL un-attempted keys on every early exit** (refusal *and* throw — `dirty` is moved out before the loop, so an `applyContentChange` throw destroys the rest un-attempted: the re-insert must be scope-exit on all paths, or iterate with a cursor and re-insert the tail) **and returns `expected<int,string>`** (the applied count — the "Ok = attempted" count needs a producer; `expected<void>` gives none). **Note**: a relocating `applyContentChange` rebinds `keyToCell_`/`cellToKey_` but leaves `Mount::mountKey`/`roots`/`owns` stale — the mount becomes unreachable for `lensUnmount`/`lensNext`/`lensStub`; noted for whoever implements the first relocating lens; `editContent` transitions `Int64`/`Double`/`Bool`-kind cells to text-typed while dirty (`setValueBits(None,0)` + `setContent`); on commit/revert, `restateContent` is **total** — `valueKind :=` the alternative's kind **and** `contentOf :=` rendered-or-`{}` in *every* branch (today only `monostate` clears spans; the scalar branches never reset `valueKind` — a cell minted `Int64` whose source later emits `std::monostate` keeps dead scalar bits); **`revertKey(key)`** = `dirty_.erase(key)` + `refresh()` **only if dirty** (lands after dirty-preserving restate); **`ProjectionLens::canApply(key)`** (default-refuse; Sql/Config override; RouterLens routes via `Mount::owns`; UnionLens first-success) — the writable probe for `lensEdit` early-refusal; routing under composites follows their `applyContentChange` paths | Provenance + the real commit-clobber + the revert primitive + the scalar-kind transition. |
| P6 | `lens_uri.cpp` | `openLensUri` refuses any unrecognized `scheme:` prefix (`^[A-Za-z][A-Za-z0-9+.-]*:`) — `meta:` included; closes `union:meta:x|…` recursion; `weird:name` file regression documented | One guard for every frontend. |
| P7 | `lens_repl.cpp` + new `zigzag/session_ops.hpp` | Gesture helpers (`sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/`sessionEdit`/`sessionRevert`) shared between `LensRepl::run` and `runIO` — `ExternalKey`-based args, `sessionNextPage(session, optional<ExternalKey> scope)` → `expected<optional<ExternalKey>>` (the materialized stub's key; `scope` = the mount key from `mountBoundary`, or `nullopt` = session-level — the REPL's unscoped `next`); **the dirty-gate lives inside them** (so the REPL inherits it; xuzz bypasses `session_ops` entirely — its own four gesture copies at `xuzz_app.cpp:691-769` — P5's dirty-preserving `restateContent` is what fixes xuzz's clobber, not the gate) | The gesture+refresh+dirty protocol exists verbatim in `lens_repl.cpp:176-237` — two copies drift; the seam is the layer below `LensRepl`. |
| P8 | `router_lens.{hpp,cpp}` | `nextStubAt(mountKey)`/`windowedFor(mountKey)` — resolve the mount owning `mountKey`, dynamic_cast its lens to `WindowedLens`, forward `nextStub`/`materializePage`; `nullopt`/`Err` when no mount or non-windowed; a relocated `mountKey` (P5's rebind) must remap `mounts_` or the mount is unreachable; `Mount::roots` is an open-time snapshot — a delegate gaining roots on refresh escapes mount-scoped routing | `RouterLens::nextStub()` is first-wins across mounts — mount-scoped `lensNext`/`lensStub` need a per-mount accessor `mounts_` doesn't expose. |

`cellKey`/`cellEq`/`cellScalar` install as **pure natives** (`makeNative`, like `linked`/
`cellText`), not `IO.*` ctors — `runIO` only performs `IO.*` effects; a pure reader of session
state can't be an effect.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay (interactive surface, separate own-fold arena today).
- Not a session stack with mount/back — D1 makes "back" retained roots.
- Not yet xuzz's picker/adopt surface — different frontend's doc.
