# vlace Lens Bridge: Lenses as First-Class Cells in a Lazy IO Language (v2)

A design specification for how a **vlace program** — not the `vlace` REPL — opens, walks, mounts,
writes back, and releases projection lenses. This is a design deliverable only: nothing described
here is implemented beyond what the compiler already does, and the signatures below are proposals to
review, not decisions shipped. Round 1 of `vortex-design-critique` is folded in; dispositions live
in `design/projects/vlace-lens-bridge-review/r1-disposition.md`.

Context: an earlier attempt exposed the entire REPL verb set as `IO.Lens*` effects, string-keyed.
It worked, but it modelled a *session state machine* — one active lens, verbs acting on it — which
is the REPL's mental model, not a lazy functional program's. This document re-designs from the
language's own idioms outward.

______________________________________________________________________

## 1. Intent

A vlace program should be able to write, in its own types:

- **open a source** as a projected root cell — `dir:`/bare path, `git:`, `sql:`, `cfg:`, `log:`,
  `journal:`, `union:` — the `openLensUri` scheme table (`lens_uri.cpp:22-97`);
- **navigate** it with the cell/dim natives it already uses on authored cells — `childrenAll`,
  `linked`, `cellText`, `dimensionNamed`;
- **mount a delegate** under a projected cell (GitLens under a worktree cell, SqlLens under a
  `.db`) and come **back** — with "back" falling out of the value model, not a stack;
- **edit and commit** projected cells on write-capable lenses (`sql:`/`cfg:` today);
- **materialize** a deferred page, and let sessions go out of scope.

The acceptance journey: `open dir → walk listing → mount git under the repo cell → read commits
→ release mount → keep walking the dir → edit+commit on a sql: lens → program ends`.

## 2. Current surface (already in the build)

| builtin | type | meaning |
|---|---|---|
| `openLens` | `String -> IO Cell` | `FilesystemLens` only; projects into the evaluator's `VortexCore`; session pinned in `Compiler::sessions_` (`compiler.hpp:223-225`) |
| `cellText` | `Cell -> String` | reads any cell's text |
| `linked` | `Cell -> Dim -> Option Cell` | posward step (no negward verb exists) |
| `childrenAll` | `Cell -> List Cell` | `d.contains` head + `d.seq` run (`compiler.cpp:1002-1028`) |
| `dimensionNamed` | `String -> Option Dim` | dim lookup |
| `words` | `String -> List String` | whitespace split |
| `promote` | `Cell -> IO Int` | promotion — the `IO.*`-effect precedent |
| `makeText`/`makeDim`/`setLink` | `String -> Cell` / `String -> Dim` / `Cell -> Dim -> Cell -> Unit` | arena verbs — see D6 |

## 3. Design decisions

### D1 — Roots are values; there is no "current lens"

Every `openLens` returns a root `Cell` backed by its own session in `sessions_`; sessions coexist.
`openLens` twice on the same source mints a *fresh, disjoint* projection — no sharing or dedup.
"Back to the previous lens" means holding the earlier root and using it — no stack, no implicit
focus. Price: one `shared_ptr<ProjectionSession>` per open; see D4.

### D2 — Ops take cells, not strings

Programs have the cell itself; keys are output only. `lensMount` takes the **mount-point cell**,
`lensNext` takes any cell naming the session (stub discovery is its own helper, §6.2),
`lensCommit` takes a session-member cell. `cellKey :: Cell -> Option String` is *output* — for
printing a projected node's `ExternalKey` — never an input, and there is no inverse `keyCell`.

### D3 — A `Cell` is a stable handle into mutable structure; reads are force-time observations

Round 1's hardest finding: projected cells are *mutable* — `lensMount`/`lensUnmount`/`lensNext`
re-enumerate the source into the same arena (`refresh()` restates content and tombstones edges,
`projection_session.hpp:68-86`). With lazy, memoizing eval, a `childrenAll dir` thunk forced
before `lensMount` caches the pre-mount run forever; the same expression forced after sees the
mount's spliced run. The rule the language keeps: **pure reads of projected cells observe the
arena as of when the thunk is forced** — a `Cell` is a stable name, its content/ranks are the
current revision. Sequencing reads against mutating ops is the program's job via `IO` bind order;
a held `List Cell` is a snapshot of a past epoch. `cellKey`/`cellText`/`childrenAll`/`linked` stay
pure-typed with this stated semantics — reads are force-time observations, not transactional
snapshots.

(`childrenAll` mints `d.contains`/`d.seq` on first use — `findOrMintDimension`,
`compiler.cpp:1018-1021` — a benign idempotent write inside a "pure" read; noted for the
mark/trail regime: arena mutations inside pure reads corrupt mint-accounting for any future
`rewind`.)

### D4 — Sessions die with the Compiler, not with cell reachability

`sessions_` is append-only (`compiler.hpp:225`): forgetting a `Cell` value releases *nothing* —
the session, its lens's OS resources (db handles, `waitableFd` watchers), and its identity maps
live to evaluation end. The bound that exists today is incidental: `Driver` builds a fresh
`Compiler` per `run`/`runIO` (`driver.cpp:92-95`), so sessions die with the *program*. Priced:
one `openLens` in a `foldl` leaks one session per iteration — bounded by program lifetime, not
free. `lensDrop :: Cell -> IO Unit` is deferred (`U-L10`): needed only if a persistent REPL ever
reuses one `Compiler`.

### D5 — `openLens` takes URIs, and refusals are `Err` values

`openLens` routes through `openLensUri` (all schemes). That widens the failure space —
`openLensUri` returns `std::expected<_, std::string>` — so `IO Cell` can't carry refusal, and
today's `Cell 0` empty-source sentinel (`compiler.cpp:1206-1215`) is a house-rule violation.
`openLens :: String -> IO (Result String Cell)` — `Err` the reason, `Ok` the root. **`meta:` and
`union:` are explicitly guarded to `EvalError`**: `openLensUri` has no `meta:` branch (frontends
resolve `meta:` via a `MetaSource` fold before calling it — `lens_repl.cpp:118-133`), and without
a guard the URI falls through to a garbage `FilesystemLens` misroute. `meta:` needs the arena to
be constructed *over* a store fold (`ArenaManifold(base)`); the evaluator's arena has no base —
refused for now.

### D6 — Arena verbs on session-owned cells are provenance-breaking

`setLink`/`makeText` write the arena on *any* `Cell`, including projected ones — bypassing the
session's `dirty_` tracking (`projection_session.cpp:262-276`) and the mount's refresh (which
tombstones lens-managed dims the next enumeration doesn't re-emit). An edit written that way is
an arena write that `lensCommit` never unprojects — and that a later `refresh()` silently erases.
Rule: **generic arena verbs must not write session-owned cells** — the correct write is
`lensEdit`, which routes through `session->editContent` so the dirty set fills and `commit`
unprojects to the source op. Guard proposal in the register (P5).

## 4. Proposed surface

| name | type | notes |
|---|---|---|
| `openLens` | `String -> IO (Result String Cell)` | `openLensUri`; `Err` on refusal; `meta:`/`union:` guarded to `EvalError` (D5) |
| `lensMount` | `Cell -> IO (Result String Cell)` | mounts the cell's delegate under it (`RouterLens::open`); `Ok` carries the **delegate root cell** — the cell to walk next |
| `lensUnmount` | `Cell -> IO (Result String Unit)` | drops the mount boundary at-or-above the cell |
| `lensNext` | `Cell -> IO (Option Cell)` | materializes the session's next `WindowedLens` stub; `Some` = the page cell |
| `lensCommit` | `Cell -> IO (Result String Int)` | `dirtyCount()` snapshot → `commit()`; `Ok` = cells written, `Err` = first refusal |
| `lensEdit` | `Cell -> String -> IO (Result String Unit)` | `session->editContent(key, text)`; `Err` for ref-bound or unkeyed cells |
| `cellKey` | `Cell -> Option String` | pure: the cell's `ExternalKey` if a projected **node** cell |

Each mutating op performs the gesture then `session->refresh()` — the arena the program reads
post-op is the re-enumerated one (the REPL does the same, `lens_repl.cpp:187-226`). Session
resolution scans `sessions_` for the one whose mint-tracking owns the cell — **not** `keyFor`
alone (that maps *node* cells only, `projection_session.hpp:45-47`); attribute cells resolve
through their owning node.

`Bool`/`Int` return shapes are deliberately `Result`/`Option`: every underlying verb returns
`std::expected<_,std::string>` and `RouterLens::open` returns the delegate root key — both
worth keeping per vlace §6.6's "failure a program should handle is a value in its type."

## 5. Worked example (target code, prelude additions P4 assumed)

```vlace
find f xs = case xs of [] => None ; (h : t) => case f h of True => Some h ; False => find f t ;
concatMap f xs = foldr (fn x => fn acc => append (f x) acc) [] xs ;

openLens "/tmp/lensj" >>= (fn dir =>
  case find (fn c => cellText c == "repo") (childrenAll dir) of
    None => putStrLn "no repo"
  ; Some repo =>
      lensMount repo >>= (fn mounted =>
        case mounted of
          Err why => putStrLn ("mount: " ++ why)
        ; Ok git =>
            putStr (foldr (fn c => fn acc => cellText c ++ acc) ""
                          (childrenAll git)) >>
            lensUnmount repo >>= (fn _ =>
              putStrLn "back to files")))
```

`find`/`concatMap` are prelude additions (P4 — vlace-source, no new native). `repo` is bound
once and reused for `lensUnmount` — the cells are the capability, not re-`find`s.

## 6. Open questions

1. **`lensMountWith`?** Mount by cell auto-delegates (`RouterLens::delegateFor`); forcing a
   specific delegate (`lensMountWith :: Cell -> String -> IO (Result String Cell)`) is deferred.
2. **Stub discovery for `lensNext`.** `WindowedLens::nextStub()` takes no arg at session level;
   stubs are projected nodes carrying `d.winstate="stub"`, discoverable by attr scan. A
   `lensStub :: Cell -> Option Cell` helper may be wanted for ergonomics.
3. **`meta:`** stays refused (D5): needs the evaluator's arena constructed over a store fold —
   `MetaSource`-shaped — plus the guard above becoming a real open.
4. **`keyCell`** — the inverse of `cellKey` — is not proposed; output-only naming.
5. **Negward climb.** `linked` is posward-only; a program holding a delegate cell can't climb to
   its mount cell — D1's retained-root model covers the common case; `lensBoundary :: Cell ->
   Option Cell` is deferred.

## 7. Proposal Register

| P | files | change | why not caller/vlace |
|---|---|---|---|
| P1 | `compiler.cpp` | `installXanadu`: `IO.LensMount`/`LensUnmount`/`LensNext`/`LensCommit`/`LensEdit`/`LensKey` ctor tags; `runIO` handlers (gesture + `refresh()`); `openLens` → `openLensUri` + `meta:`/`union:` guard; `refOfWrapped` forces the inner arg | The effect surface lives only here; cannot be expressed in vlace source. |
| P2 | `types.cpp` | Schemes for the §4 signatures | Normative typecheck needs declared builtin types. |
| P3 | `compiler.hpp` | Session-resolution helper — scan `sessions_` for the session owning a cell (mint tracking, or `keyFor` then negward-to-node) | Shared by every new op; DRY against five open-coded copies. |
| P4 | `prelude.hpp` | `find`, `concatMap` | Pure vlace source; no new native. |
| P5 | `projection_session.{hpp,cpp}` | `ownsCell(CellRef)` over `mintOrder_`/`mintCount_`; document `editContent` as the only dirty-path; `setLink`/`makeText` ownership guard (D6) | Provenance: projected-cell edits must reach `dirty_` to unproject; arena-write bypasses it. |

## 8. What this is not

- Not the `:lens` REPL verbs — those stay (interactive surface, separate own-fold arena today).
- Not a session stack with mount/back — D1 makes "back" a retained root.
- Not yet xuzz's picker/adopt surface — different frontend's doc.
