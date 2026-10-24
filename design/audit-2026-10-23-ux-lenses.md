# UX validation run: lens journeys, 2026-10-23

Run against `design/sql-to-zigzag` at the `LensRepl` integration
commit. Headless (SDL offscreen, llvmpipe), each run with its own XDG
dirs. Evidence: `/tmp/lensj/shots/*.png`, `/tmp/lensj/jl*.log`, REPL
transcripts inline below.

| Journey | Step | Affordance used | Outcome | Evidence |
| ------- | ---- | --------------- | ------- | -------- |
| J-L1 | open picker | `Shift+O` keymap default | **Finding F1** | jl1.log |
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

**F1 — lens keymap chords do not reach their verbs through the real
keypath.** `--chord shift+o`, `--chord O`, `--chord o` and `--chord b`
never fired `lens_open_source` / `lens_open` / `lens_back` in any
focus state I drove. Printable keys are consumed by whichever scope
holds keyboard focus (the document pane takes them as text — `repo`
was visibly typed into a document during one run). `F6`
(`std:xuzz/focus_toggle`) is presumably the intended gate to move the
keyboard to the slice, but after `f6` the `o` chord still did not
mount and the slice view reverted to the bound xanadoc store — worth a
dedicated pass on what `focus_toggle` does to an adopted lens view.
The actions themselves are reachable via the command table (`--do`),
and every default chord is present in `system://keymap` (test:
`SystemDocsTest.LensVerbsHaveDefaultKeymapBindings`). Proposed fix:
decide whether lens chords should fire while a document holds focus —
probably no — and make `focus_toggle` route the keyboard to the
adopted-lens scope so `O`/`B`/`P`/`Ctrl+S`/`Shift+P`/`Shift+O` work
there. Lands in `apps/xuzz/xuzz_app.cpp` (focus_toggle handler) and the
`ZigzagVisualizer::keyPressed` fallthrough.

**F2 — REPL `unmount` accepts only ExternalKeys, not paths.**
`:lens mount fs:52:794498` mounts GitLens under `repo`; a natural
`:lens unmount /tmp/lensj/repo` replies "no mount at or above" because
paths are not keys. Working as designed, but the round trip trips
users: accept path-shaped args by resolving them to the cell's fs key
in `LensRepl::run` (`apps/common/xanadu/zigzag/lens_repl.cpp`).

**F3 — picker has no text-input affordance for filtering.** `--type`
routes to the document pane, not the picker's filter (the picker is a
FocusScope, not a text scope). Filtering probably works through
per-letter key events a person produces; the harness can't type them
today — small harness gap, not a product defect.

**F4 — vprolog/vlace have no `meta:` source.** Their `:open` consumes
the store it loads, so `:lens meta:` reports "needs an open store".
URI lenses work everywhere; the meta fold exists only where a
coordinator holds a store (vquery, vpl). If vprolog is meant to
introspect the store it consulted, `openFactStore` would need to keep
a live `Store` for the fold.

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
