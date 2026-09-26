# UX Audit 2026-09-26: Real Work in Xuzz and VQuery

The first run of the [`xuzz-ux-validation`](../.claude/skills/xuzz-ux-validation/SKILL.md) workflow
against [`ux_workflow_real_work.md`](ux_workflow_real_work.md), on build 0.0.1.755
(`feature/xuzz-link-context`), headless, OpenGL only. Five journey runners covered J1+J2, J3, J4, J5
and J6; the lead merged their reports. Evidence is under `/tmp/ux-j12/`, `/tmp/ux-j3/`,
`/tmp/ux-j4/`, `/tmp/ux-j5/` and `/tmp/ux-j6/` on the machine that ran them; each finding names its
files. No product code was changed by the run.

## Verdict

No journey passes end to end. The pieces that work are the ones built most recently: the link
panel's keys and buttons, transclusion into a new page from the radial menu or Ctrl+T, Ctrl+O,
typing into an existing store, and Ctrl+Q saving everything. What blocks real work, most severe
first:

| #   | Finding                                                                                      | Journeys   | Outcome                 |
| --- | -------------------------------------------------------------------------------------------- | ---------- | ----------------------- |
| 1   | A new or empty document never settles: automation stalls, and quit queued behind it too      | J1, J3, J5 | Fail                    |
| 2   | Document text is hidden at launch in xuzz: the page draws under the tab bar and ZigZag HUD   | J1, J4, J5 | Fail                    |
| 3   | A new xanadoc is a temporary store, deleted on quit without a warning                        | J1, J5     | Fail                    |
| 4   | Xuzz registers no ZigZag navigation or editing: no home cell, no focus moves, no new slice   | J2, J4, J5 | Missing / No affordance |
| 5   | Arrow keys do nothing in xuzz: the caret cannot be moved from the keyboard                   | J4         | Missing                 |
| 6   | No session restore: open documents, caret, camera, focus and selected link are lost          | J5         | Missing                 |
| 7   | Pouches cannot be filled from the UI, forget their items on relaunch, delete on card click   | J3, J5     | Fail / No affordance    |
| 8   | Dragging a selection only reselects; the real gesture is hidden behind Alt, with no feedback | J3         | Fail                    |
| 9   | vquery cannot save a result: `-o` exports the workspace, `--in-place` changes nothing        | J6         | Fail / No affordance    |
| 10  | A store vquery saves never settles in xuzz (duplicate home cells)                            | J6         | Fail                    |
| 11  | vquery cannot follow a store's own links, drops predicates, has no text search               | J6         | Fail / Missing          |
| 12  | Key bindings: a "bound" log line for unregistered commands; eight conflicting defaults       | all        | Fail                    |

## Steps

| Journey | Step                                | Affordance used                         | Outcome       | Evidence                            |
| ------- | ----------------------------------- | --------------------------------------- | ------------- | ----------------------------------- |
| J1      | Launch with no arguments            | launch                                  | Fail (1)      | `/tmp/ux-j12/j1_launch.out`         |
| J1      | New xanadoc by menu                 | tab bar "+"                             | Fail (1)      | `/tmp/ux-j12/j1_plus.out`           |
| J1      | New xanadoc by key                  | Ctrl+N                                  | Fail (1, 3)   | `/tmp/ux-j12/`, `/tmp/ux-j5/B.log`  |
| J1      | Type into an existing store         | keyboard                                | Pass (2)      | `/tmp/ux-j12/j1_typed_existing.png` |
| J2      | New slice                           | none found                              | Missing (4)   | `/tmp/ux-j12/j2_cellradial.png`     |
| J2      | Add, name and link a cell           | none found                              | Missing (4)   | `/tmp/ux-j12/j2_cellclick.png`      |
| J3      | Select with the mouse               | press, drag, release                    | Pass          | `/tmp/ux-j3/`                       |
| J3      | Drag selection to empty space       | drag; Alt+drag not drivable             | Fail (8)      | `/tmp/ux-j3/s8.log`                 |
| J3      | Transclude to a new page            | radial menu, Ctrl+T                     | Pass          | `/tmp/ux-j3/`                       |
| J3      | Drag into a pouch                   | drag to drawer                          | Fail (7)      | `/tmp/ux-j3/s9_*`                   |
| J3      | Use a pouch item later              | card click                              | Fail (7)      | `/tmp/ux-j3/s11_*`                  |
| J4      | To the home cell                    | Alt+Home, cell click                    | Missing (4)   | `/tmp/ux-j4/`                       |
| J4      | Link panel Origin and Back          | link panel buttons, Alt+Shift keys      | Pass, partial | `/tmp/ux-j4/`                       |
| J4      | Caret moved mid-document by key     | arrows                                  | Missing (5)   | `/tmp/ux-j4/k4_right.log`           |
| J5      | Close and reopen a store            | Ctrl+W, Ctrl+O                          | Pass          | `/tmp/ux-j5/`                       |
| J5      | Typed text survives quit            | Ctrl+Q                                  | Pass          | `/tmp/ux-j5/`                       |
| J5      | View state and pouch items restored | relaunch                                | Missing (6-7) | `/tmp/ux-j5/H.log`, `I12.png`       |
| J6      | Query and refine                    | REPL                                    | Fail (11)     | `/tmp/ux-j6/`                       |
| J6      | Save the result                     | `-o`, `--in-place`; REPL has no `:save` | Fail (9)      | `/tmp/ux-j6/`                       |
| J6      | Open the result in xuzz             | launch with store path                  | Fail (10)     | `/tmp/ux-j6/xuzz-hang-bt.txt`       |

Numbers in parentheses name the finding below.

## Findings

### 1. Empty documents never settle

`Renderer::docsLoading()` (`src/renderer.cpp:142`) counts a document with no pages, or no documents
at all, as still loading. Launching on an empty store, Ctrl+N, the tab bar's "+" and closing the
last document all leave the renderer unsettled; scripted steps after them never run, and J5 saw a
queued Ctrl+Q never execute (`/tmp/ux-j12/j1_plus.out`, `/tmp/ux-j5/B.log`, `E.log`). Fix: a fully
loaded document with no pages, and an empty session, are settled; test by launching on an empty
store.

### 2. Text hidden at launch

Frames show a blank white page or the first lines under the tab bar and the ZigZag "Focus:" header,
while the accessibility dump holds the text (`/tmp/ux-j12/j1_typed_existing.png`,
`/tmp/ux-j4/s0.png`, J5's frames). Reading-first framing (`Views::frameForReading`) puts the page
top at the view's top edge, where the chrome covers it; in xuzz the ZigZag HUD covers more. Fix:
frame below the chrome, measuring the tab bar and HUD rather than assuming them.

### 3. New xanadocs are thrown away

`Session::createNewStore("")` writes to `temp_directory_path()/xudu_genesis_*`
(`apps/xudu/session.cpp:819`) and teardown deletes temporary stores (`session.cpp:274`). Only
Ctrl+S's "Preserve Temporary Xanadoc" form keeps one. Fix: on quit or close, prompt for, or
preserve, a temporary store that has operations.

### 4. No ZigZag in xuzz beyond the link panel

`apps/xudu/main.cpp` under `XUZZ_BUILD` registers only palette, bundle and confirm commands; step,
jump home, insert cell, unlink, delete, save store and the command bar exist only in
`apps/zigzag/main.cpp`. Their bindings load but name no command. Clicking a cell tile does nothing.
No UI reaches `sliceGenesis`, `updateFocusCellText` or `linkFocusAlong`. Fix: register the ZigZag
actions in xuzz through `ZigzagPresentation::dispatchAction`, routing focus through
`BridgeCoordinator`; a "New slice" radial entry and binding; a cell-text edit mode; a cell link
gesture; click-to-focus on tiles. Keep ZigZag steps on Alt-prefixed keys: the defaults also bind
bare arrows and Home, which would take the document's keys once registered.

### 5. No caret movement from the keyboard

Keys go only to the command table (`src/app.cpp`), and no caret-motion commands are registered
(`/tmp/ux-j4/k4_right.log`). Return in a document runs the ZigZag confirm action instead of
inserting a newline. Fix: caret motion and newline as keymap actions, with defaults.

### 6. No session restore

Nothing persists view state (`/tmp/ux-j5/H.log`, `FH.png`). Fix: at quit, record open stores,
per-document caret and selection, camera, ZigZag focus and the selected link as Structure in a
system xanadoc, the planned `system://activity` store being the natural home, and restore them after
the session opens. Also place a caret on open, so typing after launch lands somewhere
(`/tmp/ux-j5/J.log`).

### 7. Pouches

- No UI route fills a pouch: dragging a selection loses it when the pointer enters the drawer, whose
  pick reports the document; `pouch-drop-*` and `forge-clasp` have no bindings (`/tmp/ux-j3/s9_*`,
  `/tmp/ux-j5/p4.log`).
- `PouchManager::loadManifest()` (`apps/common/xanadu/pouch_zone.cpp:310`) rebuilds zones but not
  items, so items stored in `system://pouches` are gone after relaunch (`/tmp/ux-j3/s11_*`,
  `/tmp/ux-j5/I12.png`). An item's preview is written as new permascroll bytes, a copy.
- Clicking a card's snippet deletes the item: the dismiss tag is still set when the snippet is drawn
  (`apps/xudu/pouch_drawer.cpp` ~316). Cards cannot be dragged out.

### 8. Dragging a selection

Pressing on selected text starts a new selection; the page-making drag is Alt+drag
(`KineticTetherEngine`), unhinted, and `endDrag` spawns a page even when dropped on another page.
Dragging across the "+ Split to New Page" button inserts a page break, and the button covers the
selection (`/tmp/ux-j3/s8.log`). Fix: a press inside a selection starts the drag; spawn only over
empty space; draw a drag ghost; the split button ignores drags.

### 9–11. VQuery

- **Save:** the REPL has no `:save`. `-e … -o` runs `VQLCompiler::exportToStore`
  (`apps/common/xanadu/vql/compiler.cpp:921`) through a fresh compiler, so the result's new cells
  never arrive; it exports the whole workspace with duplicate `home` cells and drops the xanadoc's
  text. `--in-place` reports success and changes nothing. Without a permascroll the output addresses
  text never written; `-o` appends to the input's permascroll.
- **Result stores do not open:** xuzz never settles on one (`/tmp/ux-j6/xuzz-hang-bt.txt`); the
  pristine sample opens in seconds. Fixing the export fixes the trigger; xuzz should still give up
  and report on a store with several homes.
- **Finding:** `##/d.dims` returns nothing on a store linked along `d.dims`; anchor predicates are
  dropped (`#[. = "nothing"]` answers `home`); `count(#)` prints nothing; no text search; the only
  way to a cell is an operation index; `:stores` and the ascii view give one cell two ids.
- **Silence:** a missing permascroll or store path warns at most and exits 0.

### 12. Key bindings

The "bound to" debug line added in step (e) logs even when `CommandTable::rebind` found no command;
it must warn instead. Conflicting defaults: F2 (pouch, command bar), F4 (physics, palette), F5
(unlock, VQL), Ctrl+1–5 (documents, bundles), Ctrl+B (back, bundle), Ctrl+S (save, export link
package), Ctrl+Shift+S (publish, save store), Backspace (delete, delete cell). `:` and `Alt+:` do
not parse.

### Smaller findings

- Origin and Back restore the caret but not a selection (`LinkContext::noteOrigin`).
- The accessibility focus always names a ZigZag cell, never the document.
- Right-click opens the radial menu at the top right rather than at the pointer; it offers only
  formatting, transclude, page break and author.
- The link panel's buttons reflow between two layouts; its reading line can disagree with the chosen
  member.
- `apps/xudu/tenuous_tether.cpp:58` hardcodes a relative shader path, so xuzz run outside the tree
  cannot find `assets/`.
- `autoSaveSeconds` is parsed and never used (typing is written through on every keystroke, so
  nothing is lost, but the setting misleads).
- The clasp bench header draws under its slot boxes.

## Harness gaps

- `--mouse-down` cannot hold a modifier, so Alt+drag was not driven.
- Every capture waits for a settled frame, so a program that never settles yields no image; a timed
  capture would have shown findings 1 and 10.
- The build has no AccessKit ("accessibility: not in this build"): the accessibility evidence is the
  harness's own dump, and the keyboard and accessibility review was not run.

## Not validated

Typing into and saving a brand-new document (blocked by finding 1), anything on Vulkan or GLES, the
home-cell round trip (unreachable), restoring ZigZag focus (never movable), and vquery's multi-store
path.
