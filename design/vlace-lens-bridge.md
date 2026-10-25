# vlace Lens Bridge: Lenses as First-Class Cells in a Lazy IO Language (v3)

A design specification for how a **vlace program** — not the `vlace` REPL — opens, walks, mounts,
writes back, and releases projection lenses. This is a design deliverable only: nothing described
here is implemented beyond what the compiler already does, and the signatures below are proposals to
review, not decisions shipped. Rounds 1–2 of `vortex-design-critique` are folded in; dispositions
live in `design/projects/vlace-lens-bridge-review/`.

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
| `dimensionNamed` | `String -> Option Dim` | dim lookup-or-mint (`ensureDimension`, `compiler.cpp:916`) |
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
`lensNext`/`lensStub` take any cell naming the session, `lensCommit` takes a session-member cell.
`cellKey :: Cell -> Option String` is *output* — for printing a projected node's `ExternalKey` —
never an input, and there is no inverse `keyCell`.

### D3 — A `Cell` is a stable handle into mutable structure; reads are force-time observations

Projected cells are *mutable* — `lensMount`/`lensUnmount`/`lensNext` re-enumerate the source into
the same arena (`refresh()` restates content and tombstones edges,
`projection_session.hpp:68-86`). With lazy, memoizing eval, a `childrenAll dir` thunk forced
before `lensMount` caches the pre-mount run forever; a *re-evaluation* forced after sees the
mount's spliced run. The rule the language keeps: **pure reads of projected cells observe the
arena as of when the thunk is forced** — a `Cell` is a stable name, its content/ranks are the
current revision. Sequencing reads against mutating ops is the program's job via `IO` bind order;
a held `List Cell` is a snapshot of a past epoch. `cellKey`/`cellText`/`childrenAll`/`linked` stay
pure-typed with this stated semantics.

Writes-in-pure-reads: `childrenAll` mints `d.contains`/`d.seq` on first use
(`findOrMintDimension`, `compiler.cpp:1018-1021`), `dimensionNamed` mints unknown dims, and
`numericOp`/`makeText`/`makeDim`/`setLink` mint too — arena mutations inside "pure" reads corrupt
mint-accounting for any future `rewind` (foreign-mint refusal,
`projection_session.cpp:415-422`). Noted; the regime matters only if a persistent Compiler ever
shares the arena with a marking runtime.

### D4 — Sessions die with the Compiler, not with cell reachability

`sessions_` is append-only (`compiler.hpp:225`): forgetting a `Cell` value releases *nothing* —
the session, its lens's OS resources (db handles, `waitableFd` watchers — note `dir:`/`fs:`
lenses hold *no* fds, they're snapshot lenses), and its identity maps live to evaluation end.
The bound that exists today is incidental: `Driver` builds a fresh `Compiler` per
`run`/`runIO` (`driver.cpp:92-95`), so sessions die with the *program*. Priced: one `openLens`
in a `foldl` leaks one session per iteration — bounded by program lifetime, not free. `lensDrop
:: Cell -> IO Unit` is deferred (`U-L10`): needed only if a persistent REPL ever reuses one
`Compiler`. Latent hazards then: `refresh()`'s `arena.mark()`/discard is not RAII — an
exception in `forEachNode` leaks `outstandingMarks_` (`projection_session.cpp:180-250`); a
dropped session with `checkpoint()` marks leaks them too. Both matter only under U-L10.

### D5 — `openLens` takes URIs, and refusals are `Err` values

`openLens` routes through `openLensUri` (all schemes). That widens the failure space —
`openLensUri` returns `std::expected<_, std::string>` — so `IO Cell` can't carry refusal, and
today's `Cell 0` empty-source sentinel (`compiler.cpp:1206-1215`) is a house-rule violation.
`openLens :: String -> IO (Result String Cell)` — `Err` the reason (or "projected no roots" for
an empty source), `Ok` the root.

The `meta:`/`union:` hole is **fixed in `openLensUri` itself** (P6 — refuse any unrecognized
`scheme:` prefix, not at the vlace boundary): `openLensUri` has no `meta:` branch — `meta:` is a
frontend concern resolved via a `MetaSource` fold (`lens_repl.cpp:118-133`) — and without the
guard, `openLens "meta:x"` falls through to a `FilesystemLens` misroute (`lens_uri.cpp:80-96`).
A `union:meta:x|dir:/y` recurses into the same hole — fixing `openLensUri` closes both.
`union:` stays openable (it works today, `lens_uri.cpp:26-43`); note it's not a `WindowedLens`
(so `lensNext` always `Err`s on it) and `applyContentChange` is first-refusal across children.
`meta:` in vlace needs the arena constructed *over* a store fold — refused until the evaluator's
arena gains a base.

### D6 — Arena verbs on session-owned cells are provenance-breaking

`setLink` writes the arena on *any* `Cell`, including projected ones — bypassing the session's
`dirty_` tracking (`projection_session.cpp:262-276`) and the next `refresh()`'s tombstone
(which unlinks lens-managed dims the re-enumeration doesn't re-emit). Worse, `link()` maintains
both ends — `setLink scratch dim projected` mutates the projected cell's *negward* edge too.
`makeText`/`makeDim` can't write existing cells (they mint), so they're not provenance
offenders — their hazard is foreign-mint vs `rewind`, covered in D3. Rule: **generic arena
verbs must not write session-owned cells** — the correct write is `lensEdit`, which routes
through `session->editContent` so the dirty set fills and `commit` unprojects to the source op.
Guard proposal in the register (P5) — checks *both* `setLink` operands.

## 4. Proposed surface

| name | type | notes |
|---|---|---|
| `openLens` | `String -> IO (Result String Cell)` | `openLensUri`; `Err` on refusal or empty roots; `meta:` refused inside `openLensUri` (P6) |
| `lensMount` | `Cell -> IO (Result String Cell)` | `RouterLens::open` under the cell; `Ok` = **delegate root cell**; `Err` = non-RouterLens session, unowned cell, or `delegateFor` refusal. Idempotent on an already-mounted cell. |
| `lensUnmount` | `Cell -> IO (Result String Unit)` | drops the mount boundary at-or-above the cell |
| `lensNext` | `Cell -> IO (Result String (Option Cell))` | `nextStub()` → `materializePage`; `Ok (Some c)` = materialized stub cell, `Ok None` = exhaustion, `Err` = refusal |
| `lensCommit` | `Cell -> IO (Result String Int)` | `dirtyCount()` snapshot → `commit()`; `Ok` = cells written, `Err` = a refusal **with ≥0 cells already applied** (commit is per-cell, not atomic — `projection_session.cpp:280-303`) |
| `lensEdit` | `Cell -> String -> IO (Result String Unit)` | `session->editContent(key, text)`; `Err` for ref-bound or unkeyed cells. **No refresh after** — see the edit/commit rule below |
| `cellKey` | `Cell -> Option String` | pure: the cell's `ExternalKey` if a projected **node** cell (`None` for attr/authored cells) |

**Edit/commit rule (the R2 hard finding).** `editContent` writes arena text and marks `dirty_`;
a subsequent `refresh()`'s `restateContent` reverts dirty cells to *source* content
(`projection_session.cpp:189-213` — no `dirty_` exemption), and `commit()` pushes `textOf(cell)`
— the reverted text — so `lensEdit` → `refresh()` → `lensCommit` unprojects the **source**, not
the edit. The write path is self-defeating as specced. The rule: **`lensEdit` performs no
refresh** (edit is arena-local until commit — exactly what xuzz's `editContent` path does,
`xuzz_app.cpp:607`), **mutating gestures refuse while `dirty_` is non-empty** (no mount/unmount/
next between edit and commit), and P5 adds a dirty-preserving `restateContent` (skip `dirty_`
keys) — a real `projection_session` defect the same bug exists as in xuzz's `refreshIfChanged`.
The "gesture + `refresh()`" rule scopes to `lensMount`/`lensUnmount`/`lensNext` only;
`lensCommit` needs no extra refresh (its `commit()` already refreshes internally,
`projection_session.cpp:304`).

**Session resolution** scans `sessions_` for the one owning the cell — `cellToKey_` for nodes
(O(1)); attribute cells resolve to their *session* via a negward climb to the owning node, but
**never** retarget the op's key — `lensEdit`/`cellKey` on an attr cell must `Err`/`None` "no
key", not write the node's body. Ops on cells no session owns (authored cells) `Err`/`None` the
same way.

`Result`/`Option` shapes are deliberate: every underlying verb returns `std::expected` and
`RouterLens::open` returns the delegate root key — both worth keeping per vlace §6.6 ("failure
that a program should handle is a value in its type").

## 5. Worked example (self-contained; prelude isn't loaded — `kPrelude` has no production path)

```vlace
find f xs = case xs of [] => None ; (h : t) => case f h of True => Some h ; False => find f t ;

openLens "/tmp/lensj" >>= (fn opened =>
  case opened of
    Err why => putStrLn ("open: " ++ why)
  ; Ok dir =>
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
                  putStrLn "back to files")))
```

`find` is defined inline (the prelude isn't loaded by `Driver` today — wiring it is a separate
U-L work item, not this doc's `P<k>`). `opened`/`dir`/`git`/`repo` are bound once and reused —
the cells are the capability.

## 6. Open questions

1. **`lensMountWith`?** Mount by cell auto-delegates (`RouterLens::delegateFor`); forcing a
   specific delegate is deferred.
2. **`lensStub` helper.** `WindowedLens::nextStub()` needs no cell; stubs carry `d.winstate=
   "stub"` on git/log/journal lenses but `d.state="deferred"` on SqlLens — divergent attrs make
   a `lensStub :: Cell -> IO (Option Cell)` native (delegating to `nextStub()`) the right helper.
3. **`meta:`** stays refused (P6): needs the evaluator's arena constructed over a store fold —
   `MetaSource`-shaped — plus the `openLensUri` guard becoming a real open.
4. **`keyCell`** — the inverse of `cellKey` — is not proposed; output-only naming.
5. **Negward climb.** `linked` is posward-only; a program holding a delegate cell can't climb to
   its mount cell — D1's retained-root model covers the common case; `lensBoundary :: Cell ->
   Option Cell` is deferred.
6. **`union:`** stays openable but note: not `WindowedLens` (`lensNext` always `Err`), and
   `applyContentChange` is first-refusal across children — `lensEdit`/`lensCommit` on a union
   should `Err` explicitly until the semantics are decided.

## 7. Proposal Register

| P | files | change | why not caller/vlace |
|---|---|---|---|
| P1 | `compiler.cpp` | `installXanadu`: `IO.LensMount`/`LensUnmount`/`LensNext`/`LensCommit`/`LensEdit` ctor tags + `runIO` handlers (mount/unmount/next do gesture+`refresh()`; edit does **not** refresh); `openLens` → `openLensUri` + empty-roots `Err`; `refOfWrapped` forces the inner arg **and reserves `Cell`/`Dim` ctor names** (forgeable-ctor hole) | The effect surface lives only here; cannot be expressed in vlace source. |
| P2 | `types.cpp` | Schemes for §4 signatures; pin `++ :: String -> String -> String` (types `a->a->a`, eval refuses non-Str — pre-existing soundness hole) | Normative typecheck needs declared builtin types. |
| P3 | `compiler.hpp` | Session-resolution helper — scan `sessions_` for the session owning a cell (`cellToKey_` for nodes + negward climb for attrs) | Shared by every new op + the pure `cellKey` native; DRY against five open-coded copies. |
| P4 | `prelude.hpp` | `find`, `concatMap` | vlace-source additions; prelude wiring into Driver is a separate U-L item (the prelude isn't loaded by any production path today). |
| P5 | `projection_session.{hpp,cpp}` | `ownsCell(CellRef)` (`cellToKey_` + negward climb, *not* `mintOrder_` — it records keys not CellRefs); dirty-preserving `restateContent` (skip `dirty_` keys); `setLink` ownership guard covering *both* operands; `editContent` as the only dirty-path | Provenance: projected-cell edits must reach `dirty_` to unproject; the refresh-clobber is a real defect, not a doc quirk. |
| P6 | `lens_uri.cpp` | `openLensUri` refuses any unrecognized `scheme:` prefix — `meta:` included (it has no branch and silently misroutes to `FilesystemLens` today); also closes `union:meta:x|…` recursion | The right layer — one guard for every frontend, not a per-language veto. |
| P7 | `lens_repl.cpp` + new `zigzag/session_ops.hpp` | Gesture helpers (`sessionMountAt`, `sessionUnmountAt`, `sessionNextPage`, `sessionCommitAll`) shared between `LensRepl::run` and `runIO` | The mount/unmount/next/commit+refresh protocol exists verbatim in `lens_repl.cpp:176-237` — two copies drift; the seam is the layer below `LensRepl`, not the session-state machine itself. |

`cellKey` installs as a **pure native** (`makeNative`, like `linked`/`cellText`), not an `IO.*`
ctor — `runIO` only performs `IO.*` effects; a pure reader of session state can't be an effect.

## 8. What this is not

- Not the `:lens` REPL verbs — those stay (interactive surface, separate own-fold arena today).
- Not a session stack with mount/back — D1 makes "back" a retained root.
- Not yet xuzz's picker/adopt surface — different frontend's doc.
