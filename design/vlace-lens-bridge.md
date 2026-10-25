# vlace Lens Bridge: Lenses as First-Class Cells in a Lazy IO Language (v4)

A design specification for how a **vlace program** — not the `vlace` REPL — opens, walks, mounts,
writes back, and releases projection lenses. Design deliverable only: nothing here is implemented
beyond what the compiler already does; the signatures are proposals. Rounds 1–3 of
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
| `cellText` | `Cell -> String` | reads any cell's text |
| `linked` | `Cell -> Dim -> Option Cell` | posward step (no negward verb exists) |
| `childrenAll` | `Cell -> List Cell` | `d.contains` head + `d.seq` run (`compiler.cpp:1002-1028`) |
| `dimensionNamed` | `String -> Option Dim` | lookup-or-mint (`ensureDimension`, `compiler.cpp:916`) — always `Some`; the `Option` lies |
| `words` | `String -> List String` | whitespace split |
| `promote` | `Cell -> IO Int` | promotion — the `IO.*`-effect precedent |
| `makeText`/`makeDim`/`setLink` | `String -> Cell` / `String -> Dim` / `Cell -> Dim -> Cell -> Unit` | arena verbs — see D6 |

`==`/`/`/`+`/etc. type `a -> a -> T` but eval refuses non-numeric/non-Str
(`compiler.cpp:195-225`) — a pre-existing soundness hole in the same class as `++` (P2 pins
both); there is **no cell-identity predicate** — `cellEq` fills it (§4).

## 3. Design decisions

### D1 — Roots are values; there is no "current lens"

Every `openLens` returns the session's projected roots (a `List Cell` — `union:`/forest sources
emit several; a bare `Result String Cell` would orphan siblings). Sessions coexist; re-opening
the same source mints a *fresh, disjoint* projection. "Back" means holding the earlier roots —
no stack, no implicit focus. Price: one `shared_ptr<ProjectionSession>` per open; see D4.

### D2 — Ops take cells, not strings

Programs have the cell; keys are output only. `lensMount` takes the **mount-point cell**;
`lensNext`/`lensStub` take a session-member cell (they route via `mountBoundary` to that
mount's `WindowedLens` — `RouterLens::nextStub` is first-wins across mounts otherwise);
`lensCommit`/`lensRevert`/`lensEdit`/`cellKey` take a session-member cell. `cellKey :: Cell ->
Option String` is output only — no inverse `keyCell`.

### D3 — A `Cell` is a stable handle into mutable structure; reads are force-time observations

Projected cells are *mutable* — gestures re-enumerate the source into the arena (`refresh()`
restates content and tombstones edges). With lazy memoizing eval, a `childrenAll dir` thunk
forced before `lensMount` caches the pre-mount run; a re-evaluation after sees the mount's
spliced run. The rule: **pure reads of projected cells observe the arena as of when the thunk
is forced** — a `Cell` is a stable name, its content/ranks the current revision. Sequencing
reads vs mutating ops is the program's job via `IO` bind order. `cellKey`/`cellText`/
`childrenAll`/`linked`/`cellEq` stay pure-typed with this stated semantics.

Writes-in-pure-reads: `childrenAll` mints `d.contains`/`d.seq` on first use; `dimensionNamed`
mints unknown dims; `numericOp`/`makeText`/`makeDim`/`setLink` mint — arena mutations inside
"pure" reads corrupt mint-accounting for any future `rewind` (foreign-mint refusal,
`projection_session.cpp:415-422`). Noted; matters only if a persistent Compiler ever shares
the arena with a marking runtime.

### D4 — Sessions die with the Compiler, not with cell reachability

`sessions_` is append-only (`compiler.hpp:225`): forgetting a `Cell` releases *nothing* — the
session, its OS resources (db handles, watchers — `dir:`/`fs:` lenses hold no fds, they're
snapshot lenses), and identity maps live to evaluation end. The bound today: `Driver` builds a
fresh `Compiler` per `run`/`runIO` (`driver.cpp:92-95`) — sessions die with the *program*.
`lensDrop :: Cell -> IO Unit` is deferred (`U-L10`): needed only if a persistent REPL reuses
one `Compiler`. Latent hazards then: `refresh()`'s `arena.mark()`/discard is not RAII — an
exception in `forEachNode` leaks `outstandingMarks_` (`projection_session.cpp:180-250`); a
dropped session with `checkpoint()` marks leaks them too. Both matter only under U-L10.

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
(`lensNext`/`lensStub` always `Err` on it) and `applyContentChange` is first-success/
last-error across children (`union_lens.cpp:77-87`). `meta:` in vlace needs the arena
constructed *over* a store fold — refused until the evaluator's arena gains a base.

### D6 — Arena verbs on session-owned cells are provenance-breaking

`setLink` writes the arena on *any* `Cell`, including projected ones — bypassing `dirty_`
and the next `refresh()`'s tombstone. Worse, `link()` maintains both ends — `setLink
scratch dim projected` mutates the projected cell's *negward* edge too. `makeText`/`makeDim`
only mint (not provenance offenders — their hazard is foreign-mint vs `rewind`, D3). Rule:
**generic arena verbs must not write session-owned cells** — the correct write is `lensEdit`,
which routes through `session->editContent`. Guard in the register (P5) checks *both*
`setLink` operands — an authored cell negward of a projected cell still writes the projected
cell's negward slot.

## 4. Proposed surface

| name | type | notes |
|---|---|---|
| `openLens` | `String -> IO (Result String (List Cell))` | `openLensUri`; `Err` on refusal/empty roots; `meta:` refused inside `openLensUri` (P6); `Ok` = all roots (union: forests) |
| `lensMount` | `Cell -> IO (Result String Cell)` | `RouterLens::open` under the cell; `Ok` = **delegate root cell**; `Err` = non-RouterLens session, unowned cell, keyless cell, `delegateFor` refusal, or mount-while-dirty. Idempotent on an already-mounted cell. Post-mount `childrenAll` = `[delegate roots…, fs children…]` (reorders). |
| `lensUnmount` | `Cell -> IO (Result String Unit)` | drops the mount boundary at-or-above the cell; `Err` modes as mount + dirty |
| `lensNext` | `Cell -> IO (Result String (Option Cell))` | routes the cell via `mountBoundary` to *that* mount's `WindowedLens`; `Ok (Some c)` = materialized stub cell, `Ok None` = exhaustion, `Err` = refusal/dirty |
| `lensStub` | `Cell -> IO (Result String (Option Cell))` | the session's `nextStub()` key → its cell; `Ok Some` = pending stub, `Ok None` = none; `Err` = non-windowed/dirty |
| `lensCommit` | `Cell -> IO (Result String Int)` | `dirtyCount()` snapshot → `commit()`; `Ok` = cells written; `Err` = a refusal with ≥0 already applied (per-cell, non-atomic — `projection_session.cpp:280-303`); applied count unobservable — `lensDirty` covers it |
| `lensRevert` | `Cell -> IO (Result String Unit)` | restates the cell to source text + drops its key from `dirty_`; the dirty-state exit — a permanently-refusing key otherwise wedges every gesture |
| `lensEdit` | `Cell -> String -> IO (Result String Unit)` | `session->editContent(key, text)`; `Err` for ref-bound, unkeyed, or non-string-kind cells. **No refresh after** — see the edit/commit rule |
| `lensDirty` | `Cell -> IO Int` | `dirtyCount()` for the resolved session |
| `cellKey` | `Cell -> Option String` | pure: `ExternalKey` if a projected **node** cell (`None` for attr/authored cells) |
| `cellEq` | `Cell -> Cell -> Bool` | pure: identity on `CellRef` — `==` on cells crashes today (`compiler.cpp:195-225`) |

**Edit/commit rule.** `editContent` writes arena text + `dirty_`; `refresh()`'s
`restateContent` reverts dirty cells to source content — `lensEdit` → refresh → `lensCommit`
unprojects the *source*, not the edit. Rules: **`lensEdit` performs no refresh** (edit is
arena-local until commit — xuzz's `editContent` precedent, `xuzz_app.cpp:607`); **mutating
gestures refuse while the resolved session's `dirty_` is non-empty** (per-session; `openLens`
is exempt — new session, disjoint cells); P5 adds a dirty-preserving `restateContent` (skip
`dirty_` keys) **and** makes `commit()` re-insert *all* un-attempted keys on refusal (today it
re-inserts only the failing one — the rest drop from `dirty_` and silently lose their edits,
`projection_session.cpp:283-293`); `commit()`'s internal `refresh()` runs on success only
(`:304`). The "gesture + `refresh()`" rule scopes to `lensMount`/`lensUnmount`/`lensNext` only.
`lensRevert` is the dirty-state exit — without it a refusing key wedges the session.

**Session resolution** scans `sessions_` for the one owning the cell — `cellToKey_` for nodes
(O(1)); attr cells climb negward `d.contains` to the owning node for *session* discovery, but
the op's key stays the cell's own or `Err`/`None` "no key" (never retargets the node's body —
all key-needing ops, not just `lensEdit`). Authored cells → `Err`/`None` the same way.

`Result`/`Option` shapes are deliberate: every underlying verb returns `std::expected`, and
`RouterLens::open` returns the delegate root key — vlace §6.6: "failure that a program should
handle is a value in its type."

## 5. Worked example (self-contained — `kPrelude` has no production path)

```vlace
find f xs = case xs of [] => None ; (h : t) => case f h of True => Some h ; False => find f t ;
foldr f z xs = case xs of [] => z ; (h : t) => f h (foldr f z t) ;

openLens "/tmp/lensj" >>= (fn opened =>
  case opened of
    Err why => putStrLn ("open: " ++ why)
  ; Ok roots =>
      case roots of
        (dir : _) =>
          case find (fn c => cellText c == "repo") (childrenAll dir) of
            None => putStrLn "no repo"
          ; Some repo =>
              lensMount repo >>= (fn mounted =>
                case mounted of
                  Err why2 => putStrLn ("mount: " ++ why2)
                ; Ok git =>
                    putStr (foldr (fn c => fn acc => cellText c ++ acc) ""
                                  (childrenAll git)) >>
                    lensUnmount repo >>= (fn _ =>
                      putStrLn "back to files"))
        [] => putStrLn "empty")
```

`find`/`foldr` are defined inline (the prelude isn't loaded by `Driver`; `LensVlaceGate` does
the same, `lens_vlace_gate_test.cpp:111`). `dir`/`git`/`repo` are bound once and reused.

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

## 7. Proposal Register

| P | files | change | why not caller/vlace |
|---|---|---|---|
| P1 | `compiler.cpp` | `installXanadu`: `IO.LensMount`/`LensUnmount`/`LensNext`/`LensStub`/`LensCommit`/`LensRevert`/`LensEdit`/`LensDirty` ctor tags + `runIO` handlers; `cellKey`/`cellEq` pure natives (`makeNative`); `openLens` → `openLensUri` + `List Cell` roots + empty-roots `Err`; `refOfWrapped` forces inner arg **and reserves `Cell`/`Dim` ctor names** (ECtor + registerData both refuse — forgeable-ctor hole closed at construction) | The effect surface lives only here. |
| P2 | `types.cpp` | Schemes for §4; pin `++ :: String -> String -> String` **and** `==`/`/=`/`<`/`>` to comparable ground types (eval refuses non-numeric/non-Str — pre-existing hole; `++`'s pin reconciles vs spec §5.1/§7.4 which assigns it to `std:fun/append` — a spec deviation to flag) | Normative typecheck needs declared types. |
| P3 | `compiler.hpp` | Session-resolution helper — scan `sessions_` for the owning session (`cellToKey_` for nodes + negward `d.contains` climb for attrs); shared by ops + `cellKey` | Shared by every op; DRY against five open-coded copies. |
| P4 | `prelude.hpp` | `find` only (concatMap dropped — unused) | Prelude wiring into Driver is a separate U-L item; the prelude isn't loaded by any production path today. |
| P5 | `projection_session.{hpp,cpp}` | `ownsCell(CellRef)` (`cellToKey_` + negward climb, not `mintOrder_` — records keys not CellRefs); **dirty-preserving `restateContent`** (skip `dirty_` keys — fixes xuzz's `refreshIfChanged` clobber too, `xuzz_app.cpp:615`); **`commit()` re-inserts ALL un-attempted keys on refusal** (today only the failing one — silent edit loss); `editContent` refuses non-string-kind cells (scalar span-run hazard); `setLink` guard covers **both** operands | Provenance + the real commit-clobber defect — not a doc quirk. |
| P6 | `lens_uri.cpp` | `openLensUri` refuses any unrecognized `scheme:` prefix (`^[A-Za-z][A-Za-z0-9+.-]*:`) — `meta:` included (silently misroutes today); closes `union:meta:x|…` recursion; `weird:name` file regression documented | The right layer — one guard for every frontend. |
| P7 | `lens_repl.cpp` + new `zigzag/session_ops.hpp` | Gesture helpers (`sessionMountAt`/`sessionUnmountAt`/`sessionNextPage`/`sessionCommitAll`/`sessionEdit`/`sessionRevert`) shared between `LensRepl::run` and `runIO` — **the dirty-gate lives inside them** (so the REPL inherits it and the xuzz clobber closes too) | The gesture+refresh+dirty protocol exists verbatim in `lens_repl.cpp:176-237` — two copies drift; the seam is the layer below `LensRepl`. |

`cellKey`/`cellEq` install as **pure natives** (`makeNative`, like `linked`/`cellText`), not
`IO.*` ctors — `runIO` only performs `IO.*` effects; a pure reader of session state can't be
an effect.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay (interactive surface, separate own-fold arena today).
- Not a session stack with mount/back — D1 makes "back" retained roots.
- Not yet xuzz's picker/adopt surface — different frontend's doc.
