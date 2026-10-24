# Anything-to-ZigZag Lenses: UX Validation Contract

## Purpose

A person should be able to open a file, directory, database, config, log,
journal or store as a ZigZag projection — browse it, hop into a deeper
lens, come back out, and close it — from `xuzz`'s interface and from any
Vortex frontend's prompt or command line. This document is the contract
the [`ux-validation`](../.claude/skills/ux-validation/SKILL.md) workflow
validates against, in the format of
[real-work journeys](ux_workflow_real_work.md). It describes journeys a
user takes and what counts as done; it is not evidence that any of them
works today.

A journey passes only when every step is reachable through the program's
own interface: on-screen controls, the `system://keymap` default
bindings (`Shift+O` lens.open-source, `O` lens.open, `B` lens.back,
`P` lens.next-page, `Ctrl+S` lens.commit, `Shift+P` lens.promote),
pointer gestures, or a visible REPL prompt (the shared `:lens` verbs).
The automation options stand in for a person's hands. Each REPL journey
also names the programmatic path — `--lens <uri>` prints the projection
and exits, and `openLensUri` is the C++ entry — because a lens that only
works inside one program is half the point.

## Journeys

### J-L1. Find a git worktree in the lens file browser and open it

1. Launch `xuzz` with no arguments.
1. Open the lens file browser with `Shift+O`. A modal picker lists the
   current directory.
1. Navigate with the arrow keys to a directory holding a git worktree
   and pick it (`Ctrl+Return` on the dir, or `Return` once inside).
1. The projection of that directory fills the slice view: the repo's
   files as cells under a dir root.

Evidence: frames of the picker and the adopted projection; the
accessibility tree's cell list naming the repo's files and
`(Focused)` on the dir cell.

Watch for: the picker leaving a modal behind after the pick (input
swallowed, commands refused); the pick landing on the wrong directory;
a picker that can only start in one fixed directory.

### J-L2. Hop into GitLens and back, landing on the last location

1. From the file browser's projection, focus the worktree's dir cell.
1. Press `O` (lens.open). The delegate lens mounts under the cell:
   the repo's refs, the HEAD page and the commit DAG as cells.
1. Navigate the commits (arrows or `P` to materialize the next page).
1. Press `B` (lens.back). The mount unmounts; focus lands back on the
   dir cell it came through — the reader's last location in the file
   browser, not the top of the projection.

Evidence: frames of the mounted git root, a commit page, and the
post-back view; the accessibility tree's `(Focused)` cell before and
after the hop out.

Watch for: focus landing on a stale or unrelated cell after back; the
mount's cells staying live after unmount; a back that exits the whole
lens rather than the one hop.

### J-L3. Open a config file's lens and walk its members

1. `Shift+O`, navigate to a `.json` file, pick it.
1. The file opens directly as a `ConfigLens` projection (the router's
   delegate, opened rather than mounted): members as cells.
1. Navigate to a leaf member and read its value cell.

Evidence: frames; the accessibility tree naming `cfg.file` root and the
member keys.

Watch for: the file opening as a plain FilesystemLens cell instead of
its delegate; members minted but unreachable by navigation.

### J-L4. Open a database and browse its first table

1. `Shift+O`, navigate to a `.db` file, pick it.
1. The file opens as `SqlLens`: connection root, schema tables, and a
   live page of rows for the first base table.
1. `P` materializes the next page of rows; `Ctrl+S` commits a dirty
   edit (after editing a value cell) and the change reaches the file.

Evidence: frames of the page cells; `sqlite3`/`psql` showing the
committed value afterward.

Watch for: the `.db` opening as an opaque file cell; rows that stop at
the first page; a commit that reports success without writing.

### J-L5. Introspect a local store with MetaLens

1. Launch `vquery` or `vpl` on a store.
1. At the prompt, `:lens meta:` (or `:lens meta:<label>`) — the
   loaded store's own structure projects: dimension census and cell
   indexes as cells.
1. `:lens close` drops the projection.

Evidence: REPL transcript showing the `meta.store` root with its
`meta.index` children, keyed `meta:<label>`.

Watch for: a projection of a *copy* (a second `CellID` space) rather
than project-by-reference over the source fold; `meta:` silently
falling back to the filesystem; no way to tell which store the label
bound.

### J-L6. Open, navigate and close a lens from any Vortex frontend

1. In `vquery`, `vpl`, `vprolog` and `vlace` alike, type `:lens <uri>`
   for a directory, a `.json`, a `.db` and a `git:` URI.
1. Where the lens is a router (`dir:`), `:lens mount <key>` mounts a
   delegate, `:lens unmount <key>` takes it back down.
1. Where it is windowed (log/journal/SQL), `:lens next` materializes
   the next page.
1. `:lens commit` writes dirty cells; `:lens close` drops the session
   and the prompt returns to its own language.
1. The same runs non-interactively: `<frontend> --lens <uri>` prints
   the projection tree and exits 0.

Evidence: a transcript per frontend showing the same verbs producing
the same projection shapes; the `--lens` raster identical to
`xuzz --headless --lens` for a shared URI.

Watch for: a frontend where the verb is missing or spells differently;
a mount that works in xuzz but not the REPL; a close that leaves the
session printing lens output.

## Findings taxonomy

As in the real-work contract: **Pass**, **Fail**, **No affordance**
(reachable only by flag, script or prepared file), **Missing**
(intended but absent), **Harness gap** (the validator cannot perform a
gesture a person can). A step reachable only through `--lens` in a
journey that names a REPL or picker step is a finding, not a pass.
