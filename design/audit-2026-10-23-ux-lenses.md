# UX validation run: lens journeys, 2026-10-23

Run against `design/sql-to-zigzag` at the `LensRepl` integration
commit. Headless (SDL offscreen, llvmpipe), each run with its own XDG
dirs. Evidence: `/tmp/lensj/shots/*.png`, `/tmp/lensj/jl*.log`, REPL
transcripts inline below.

| Journey | Step | Affordance used | Outcome | Evidence |
| ------- | ---- | --------------- | ------- | -------- |
| J-L1 | open picker | `Shift+O` keymap default | Pass (after F1 fix) | jl1.log |
| J-L1 | open picker | `lens_open_source` action | Pass | jl1_picker.png |
| J-L1 | navigate + pick dir | arrows, `Ctrl+Return` | Pass | jl1_opened.png, `/tmp/lensj/data.db -> 14 cells adopted` |
| J-L2 | mount GitLens | `lens_open` action | Pass | jl2.log: `git.repo`, `git.ref`, `git.page`, `c3..c1` cells |
| J-L2 | back to file browser | `lens_back` action | Pass | jl2_back.png; a11y `Cell #47: repo [dir] (Focused)` — the mount cell, i.e. last location |
| J-L3 | pick `.json` → ConfigLens | `--lens <file>` + delegate open | Pass | jl3.log: `cfg.file`, `cfg.val` cells |
| J-L4 | pick `.db` → SqlLens | `--lens <file>` + delegate open | Pass | jl4.log: `sql.conn`, `sql.table`, `sql.col`, `sql.query`, `sql.page` |
| J-L4 | next page / commit | `P` / `Ctrl+S` chords | **Finding F1** (chord path) | jl4.log |
| J-L5 | MetaLens on a store | `vquery` `:lens meta:` | Pass | transcript: `meta.store` → `meta.index` ×2 |
| J-L6 | `:lens` open/mount/unmount/next/commit/close | REPL prompt | Pass | transcript below |
| J-L6 | `--lens <uri>` | CLI flag | Pass | identical raster on all four frontends |

## Findings

**F1 — FIXED (`b89abe11`).** `bindZigzag` ran `bindXanaduStore` on every
keyboard hand-off into the pane (F6), view-mode switch and document
activation — wiping the adopted lens document; `adopted_` stayed set so
the contributor never re-adopted. The chords were always fine; the view
they operated on was gone. `bindZigzag` now skips the rebind while a
lens surface (or the picker's transient doc) owns the view. Verified:
`F6` → `O` mounts GitLens under the repo cell through the real keypath;
`B` unmounts and lands focus on the mount cell.

**F2 — FIXED (`87cff41b`).** `:lens mount`/`unmount` resolve
path-shaped args to `fs:<dev>:<ino>` keys via `stat()`.

**F3 — FIXED (this commit).** The picker now takes text: `textTyped`
appends to a basename filter composed into `FilteredFilesystemLens`
(dirs always survive so the listing stays navigable); `Backspace`
edits the filter while one is typed, else ascends.

**F4 — FIXED (this commit).** `vprolog`'s `:lens meta:` lazily opens a
`Store` from the last `:open` path and keeps it alive for the fold;
URI lenses project into `compiler.core()` so projected cells are cells
goals can unify over.

**F5 — NEW: `vpl :rank` cannot reach lens cells.** `bindRank` resolves
dimensions only on the open store's manifold; a lens projects into the
coordinator's arena. `:rank d.contains` with a lens open replies "No
dimension named". The idiomatic VPL bridge is a rank bound over
`coordinator.arena()` — needs the federation/proxy seam wired, left as
a follow-up.

**F6 — NEW: vprolog goals cannot name a lens cell.** Cells are
first-class terms, but no builtin (`lensroot/1`, `extkey/2`) binds the
projection's root or maps keys to refs, so a goal cannot reach the
projected cells unaided. Small compiler addition when wanted.

**F7 — NEW: vlace has no core seam.** Its Driver holds no store or
VortexCore; `:lens`/`--lens` project into a private fold. A
`lens("uri")` builtin returning a cell record is the idiomatic bridge
if vlace should introspect sources.

## Transcripts

`vquery` (abbreviated; fixture `/tmp/lensj` = git repo + `settings.json` + `data.db`):

```
vql> :lens /tmp/lensj
lens: /tmp/lensj -> 64 cells, 1 root(s)
lensj  [dir, inode/directory]  fs:52:794497
  data.db  [file, application/vnd.sqlite3]  fs:52:794567
  repo  [dir, inode/directory]  fs:52:794498
    .git ...
vql> :lens mount fs:52:794498
lens: /tmp/lensj -> 70 cells, 1 root(s)
    /tmp/lensj/repo  [git.repo, ]  git:/tmp/lensj/repo
      refs/heads/main  [git.ref, ]
      p:0  [git.page, ]
        c3  [git.commit, ]
        c2  [git.commit, ]
        c1  [git.commit, ]
vql> :lens close
lens: closed
vql> :lens meta:
meta:  [meta.store, ]  meta:
  dims  [meta.index, ]  meta::dims
  cells  [meta.index, ]  meta::cells
```

`--lens` programmatic raster (same tree shape on vquery, vpl, vprolog,
vlace and `xuzz --headless --lens`):

```
$ vpl --lens /tmp/lensj/data.db
lens: /tmp/lensj/data.db -> 14 cells, 1 root(s)
sqlite:/tmp/lensj/data.db  [sql.conn, ]
  t  [sql.table, ]
  SELECT "id","v" FROM "t" ORDER BY "id" ASC NULLS LAST  [sql.query, ]
```

## Not validated

- `Ctrl+S` commit from the UI (needs a cell edit gesture first —
  covered at engine level by `SqlLensTest`/`PgBackendTest`).
- `P` next-page from the UI — `lens_next_page` ran without visible
  change on the small fixture (no stubbed page existed).
- macOS/Windows `FileWatch` variants, real-display rendering.
