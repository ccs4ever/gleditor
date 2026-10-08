# UX Audit 2026-10-01: Real Work in Xuzz and VQuery, Rerun

The second run of the [`xuzz-ux-validation`](../.claude/skills/ux-validation/SKILL.md) workflow
against [`ux_workflow_real_work.md`](ux_workflow_real_work.md), repeating J1–J6 after the fixes for
[the first audit](audit-2026-09-26-ux-real-work.md). Build 0.0.1.806 (source at `0fbe106` on
`feature/xuzz-link-context`; the version string names its parent, `9394c6e`, because the binary was
linked before the commit was made and `make` had nothing left to rebuild), headless, OpenGL only.
Five journey runners covered J1+J2, J3, J4, J5 and J6 (J5 and J6 after the first three); the lead
merged their reports and reran the two J6 findings that disagreed with other runners. Evidence is
under `/tmp/ux2-j12/`, `/tmp/ux2-j3/`, `/tmp/ux2-j4/`, `/tmp/ux2-j5/` and `/tmp/ux2-j6/` on the
machine that ran them. No product code was changed by the run; the one harness gap fixed since is
noted below.

## Verdict

J1 through J5 pass end to end, through menus, key bindings and the pointer. Every finding of the
first audit that blocked them is fixed: new and empty documents settle, text is framed below the
chrome, untitled xanadocs are kept, xuzz edits and navigates slices, the caret moves by key, the
whole session comes back after a relaunch with no store argument, pouches fill, keep and give back
their items, and dragging a selection makes a transclusion with a drag ghost. J5's frame after
relaunch is byte-identical to the frame before quit.

J6 passes only on a store with one hypertime branch. What still blocks real work, most severe first:

| #   | Finding                                                                                  | Journeys | Outcome        |
| --- | ---------------------------------------------------------------------------------------- | -------- | -------------- |
| 1   | On a store with more than one branch, `##` is a fake cell, so every `##/...` query fails | J6       | Fail           |
| 2   | A result store opens in xuzz with its `d.result` rows off screen                         | J6       | Fail           |
| 3   | Clicking the ZigZag minimap's background blanks the document's text                      | J4       | Fail           |
| 4   | `find` ignores document prose; a bare `contains(a, b)` prints nothing                    | J6       | Missing / Fail |
| 5   | The pouch drawer and its cards are absent from the accessibility tree                    | J3       | Missing        |
| 6   | The link panel keeps saying "reading: left member" after Cross or Enter                  | J4       | Fail           |
| 7   | vquery with a permascroll path that does not exist warns and exits 0                     | J6       | Partial        |

## Steps

| Journey | Step                                      | Affordance used                                                  | Outcome     | Evidence                                                   |
| ------- | ----------------------------------------- | ---------------------------------------------------------------- | ----------- | ---------------------------------------------------------- |
| J1      | Launch                                    | launch with a store path                                         | Pass        | `/tmp/ux2-j12/j1_ctrln/00_launch.png`                      |
| J1      | New xanadoc by control                    | tab bar "+"                                                      | Pass        | `/tmp/ux2-j12/j1_tabplus/01_after_click.png`               |
| J1      | New xanadoc by key                        | Ctrl+N                                                           | Pass        | `/tmp/ux2-j12/j1_ctrln/01_after_chord.png`                 |
| J1      | New xanadoc by menu                       | radial menu File > New xanadoc                                   | Pass        | `/tmp/ux2-j12/j1_radial/03_after_newdoc.png`               |
| J1      | Type, then quit and confirm saved         | keyboard, Ctrl+Q, `xudu-dump`                                    | Pass        | `/tmp/ux2-j12/j1_*/`                                       |
| J2      | New slice                                 | Ctrl+Alt+N, radial menu File > New slice                         | Pass        | `/tmp/ux2-j12/j2_chord2/01_new_slice.png`                  |
| J2      | Edit, insert, mark and link cells         | E, N, D, M, Alt+Home, L                                          | Pass        | `/tmp/ux2-j12/j2_chord2/09_linked.png`                     |
| J2      | Structure operations stored               | Ctrl+Q, `xudu-dump`                                              | Pass        | `/tmp/ux2-j12/j2_chord2/`                                  |
| J3      | Select with the mouse                     | press, drag, release                                             | Pass        | `/tmp/ux2-j3/s3/02_selected.ppm`                           |
| J3      | Drag selection to empty space             | press inside the selection, drag                                 | Pass        | `/tmp/ux2-j3/s1/d_middrag1.png`, `f_afterdrop.png`         |
| J3      | Drag selection onto a page                | drag to another point on an open page                            | Pass, part  | `/tmp/ux2-j3/s3/op_afterdrop.ppm`                          |
| J3      | Selection into a pouch                    | drag into the drawer; Ctrl+D                                     | Pass        | `/tmp/ux2-j3/s3/drag_afterdrop.png`                        |
| J3      | Pouch items survive a relaunch            | F2 after relaunch                                                | Pass        | `/tmp/ux2-j3/s3/13_relaunch_pouch_open.png`                |
| J3      | Insert a pouch item                       | the card's +                                                     | Pass        | `/tmp/ux2-j3/s3/16_after_plus_click.ppm`                   |
| J4      | To the home cell and back                 | Alt+Home, F6, tile click; Escape, F6, page click                 | Pass        | `/tmp/ux2-j4/rk_*.png`, `f6_*.png`                         |
| J4      | Camera, caret and selection kept          | the same round trips                                             | Pass        | `/tmp/ux2-j4/sel_s2_back.png`                              |
| J4      | Caret by key                              | arrows, Home, End, Ctrl+arrows, Ctrl+Home, Ctrl+End, Shift+Right | Pass        | `/tmp/ux2-j4/caret_*.log`                                  |
| J4      | Keys go to the pane with focus            | typing and M in each pane                                        | Pass        | `/tmp/ux2-j4/kb_*`                                         |
| J4      | Link panel round trip                     | Alt+Shift+N, X, Return, O, B, D                                  | Pass (6)    | `/tmp/ux2-j4/xr_*.png`, `dm_*.png`                         |
| J4      | Background of the minimap                 | click                                                            | Fail (3)    | `/tmp/ux2-j4/mm_s1.png`                                    |
| J5      | Work, quit, relaunch with no store        | Ctrl+N, Ctrl+Alt+N, Ctrl+D, wheel, Ctrl+Q, launch                | Pass        | `/tmp/ux2-j5/16_final_before_close.png`, `17_relaunch.png` |
| J5      | Documents, caret, selection, camera back  | relaunch                                                         | Pass        | `/tmp/ux2-j5/17_relaunch.png`                              |
| J5      | Slice and focused cell back               | relaunch                                                         | Pass        | `/tmp/ux2-j5/` accessibility dump                          |
| J5      | Pouch item back; card click keeps it      | Ctrl+\\, card click                                              | Pass        | `/tmp/ux2-j5/18_pouch_after_relaunch.png`                  |
| J5      | Typing resumes at the restored caret      | keyboard                                                         | Pass        | `/tmp/ux2-j5/19b_a11y.log`                                 |
| J5      | Stores intact, activity store append-only | `xudu-dump` before and after                                     | Pass        | `/tmp/ux2-j5/pre_*.dump`, `post_*.dump`                    |
| J6      | Query, refine, `:save`                    | REPL                                                             | Pass        | `/tmp/ux2-j6/09-repl-journey/transcript.raw`               |
| J6      | `-e … -o`, `--in-place`                   | command line                                                     | Pass        | `/tmp/ux2-j6/01-e-o/`, `02-inplace/`                       |
| J6      | Predicates, `count`, `find` on cells      | REPL                                                             | Pass        | `/tmp/ux2-j6/05-repl-predicates/transcript.raw`            |
| J6      | `##/d.dims` on a multi-branch store       | REPL, `-e`                                                       | Fail (1)    | `/tmp/ux2-j6/04-repl-links/transcript.raw`                 |
| J6      | `find` on document prose; bare `contains` | REPL                                                             | Missing (4) | `/tmp/ux2-j6/05-repl-predicates/transcript.raw`            |
| J6      | Open the result store in xuzz             | launch with the store path                                       | Pass        | `/tmp/ux2-j6/09-repl-journey/stdout.txt`                   |
| J6      | See its rows on `d.result`                | look at the frame                                                | Fail (2)    | `/tmp/ux2-j6/09-repl-journey/frame.png`                    |
| J6      | Missing store, missing permascroll        | command line                                                     | Pass, (7)   | `/tmp/ux2-j6/07-errors/`                                   |

Numbers in parentheses name the finding below.

### First audit's findings

| #   | First audit                    | Now                                                            |
| --- | ------------------------------ | -------------------------------------------------------------- |
| 1   | Empty documents never settle   | Fixed (J1, J5)                                                 |
| 2   | Text hidden under the chrome   | Fixed (J1, J4)                                                 |
| 3   | New xanadocs thrown away       | Fixed: kept as `untitled-*` when they hold text (J1, J5)       |
| 4   | No ZigZag in xuzz              | Fixed (J2, J4); result rows framing is new finding 2           |
| 5   | No caret movement by key       | Fixed (J4)                                                     |
| 6   | No session restore             | Fixed (J5)                                                     |
| 7   | Pouches                        | Fixed (J3, J5); accessibility is new finding 5                 |
| 8   | Dragging a selection           | Fixed, with a drag ghost (J3)                                  |
| 9   | vquery cannot save             | Fixed: one writer behind `-o`, `:save`, `--in-place` (J6)      |
| 10  | Result stores never settle     | Fixed (J6)                                                     |
| 11  | Links, predicates, text search | Predicates, `count`, cell `find` fixed; see findings 1 and 4   |
| 12  | Key bindings                   | Fixed: no conflicts or unparsed chords in the binding log (J2) |

## Findings

### 1. `##` on a store with more than one branch

On `beams/01_one_to_many` and `core_hypertext/unified_store`, both with a second branch,
`vquery <store> -e '##'` prints the store's label instead of `home`, and every `##/...` query
answers nothing. Reproduced by the lead. `MultiStoreCoordinator::addStore()`
(`apps/common/xanadu/vql/multi_store.cpp` ~176–206) folds the manifold at `Store::latest()`, which
names the greatest branch; a second branch holding one text insert outranks the branch with all the
structure, the fold has no home, and `addStore()` then mints a cell holding the label and calls it
home. A document with a second branch is what J1–J5's ordinary work makes, so this blocks querying
real stores. Fix: fold at a version whose chain holds the home (`Store::structureHeads()`, beside
`latest()`), preferring the one on the primary current version's chain; refuse loudly, naming the
store, when no version has a home, rather than inventing one.

### 2. Result rows off screen in xuzz

A result store from `:save` opens and settles with one home, and the accessibility tree lists its
rows, but the frame shows only the home tile pinned at the right edge, its `d.result` link running
off the window (`/tmp/ux2-j6/09-repl-journey/frame.png`). Alt+Home, F6 and Ctrl+Alt+2 give the same
frame. The runner read this as the first audit's finding 4; J2 and J4 show that is fixed, so this is
framing: `fitViewToHome` picks `d.result` but the camera does not bring its neighbours on screen.
Fix: after fitting dimensions, frame the home and its first ranks inside the ZigZag pane, in
`apps/zigzag/zigzag_visualizer.cpp`.

### 3. Minimap background click blanks the document

Clicking inside the minimap but off its tile (pixel 125,430 in `slice_then_xanadoc`) picks an
overlay tag with no cell, which `ZigzagVisualizer::picked` rightly ignores, yet the document's
glyphs vanish and stay gone on later settled frames; reproduced twice (`/tmp/ux2-j4/mm_s0.png`,
`mm_s1.png`, `r1_s1_cellclick.png`). Not root-caused; start at the minimap's draw and tag path
(`apps/zigzag/zigzag_visualizer.cpp` ~1787) and whatever the click changes in the document's draw
state.

### 4. `find` and `contains`

- `find("text")` searches cell text only (`vql_engine.cpp` ~504): `find("Xanadoc")` misses prose the
  store's own document shows. Searching documents reached through an operation-handle cell would
  make it the text search J6 asks for.
- `contains(a, b)` works as a predicate (`#[contains(., "x")]`) but at the top level prints nothing
  and raises nothing (`VQLEngine::evaluatePath`'s function chain, ~483–600). Evaluate it, or report
  that it is predicate-only.

### 5. Pouches are invisible to accessibility

With the drawer open and items in it, `--dump-a11y` lists the documents and the open-documents list,
no drawer, zone or card (`/tmp/ux2-j3/s3/`). A screen-reader user cannot find what a pouch holds or
reach the card's +. Fix: have the window's accessibility tree ask `apps/xudu/pouch_drawer.cpp` for a
group per zone and a named node per card with its actions.

### 6. Link panel reading line

After Cross (Alt+Shift+X) and Enter (Alt+Shift+Return) move to the right member, the panel still
reads "reading: left member 1" (`/tmp/ux2-j4/xr_s2_entered.png`); the Left/Right, occurrence and
origin values are right. The first audit noted the reading line disagreeing with the chosen member;
it still does.

### 7. Missing permascroll

`vquery --permascroll <missing path>` warns and exits 0, as `xudu-dump` does when it renders
structure without text. For a query whose answer is text this is a silent empty answer; exiting
non-zero when the path was named and does not exist would match the store-path behaviour, which now
fails loudly with exit 1.

### Smaller findings

- After clicking a pouch card in J5, the second tab's label changed from "1" to "3"; nothing was
  lost. Not investigated.
- In J2, D after N inserts beside the newly focused cell, and M then L from home adds a second `d.1`
  link to that cell. Whether that is the intended rule is a design question, not a defect.
- Not rechecked: the link panel's buttons reflowing between layouts, the clasp bench header under
  its slot boxes.

## Harness gaps

- **`--headless` dropped scripts** of `--chord`, `--capture`, `--drag`, mouse and wheel steps,
  `--screenshot` and `--dump-a11y`: the batch path kept its own list of six script options, saved,
  and exited 0 with no output. Three runners lost time to it. Fixed after the run in `6082945`:
  `gleditor::wantsFrames()` reads the same list as `readAutomationScript()`.
- `--dump-a11y` is not a step: it prints once, when the script has finished and the frame settled,
  and not at all when the script's last step quits (Ctrl+Q). Make it a script step like `--capture`.
- `--select` always selects in document 0 (`src/renderer.cpp` ~903); a selection in another document
  needs a drag. Resolve it against the focused document.
- `--type "[...]"` reads a leading bracket as decorations, so bracketed literal text cannot be
  typed. Give it an escape.
- The build has no AccessKit; the accessibility evidence is the harness's own dump, and the keyboard
  and accessibility review role was not run.

## After the run

Each finding was rerun by the lead before it was fixed. Two did not survive the rerun: the runners
had misread their own frames.

| #   | Outcome                                                                                                                                              | Commit    |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | --------- |
| 1   | Fixed: a guessed version that folds no home falls back to a structure head                                                                           | `1288f7e` |
| 2   | Fixed: a store that is all slice opens on its slice, ZigZag holding the keyboard                                                                     | `46cfe2e` |
| 3   | Not a defect: the click panned the view, as a minimap does, and the one line went under the header; scrolling back shows it                          | —         |
| 4   | Fixed: `find` reads current documents' lines, linked along `d.source`; bare `contains` answers; an unknown function is refused by name               | `fc30ef2` |
| 5   | Fixed: the drawer contributes a list per zone and a card per item, with Remove and Go to where it came from; its revision follows `system://pouches` | `5929b08` |
| 6   | Not a defect: after Enter the frame reads "right member 1"; after Cross the reader is still on the left text, which is what "left member 1" says     | —         |
| 7   | Fixed: a named permascroll that does not exist is refused, exit 1, nothing created                                                                   | `4546563` |

Finding 2 was also narrower than reported: after Alt+Home the result row was on screen; only the
launch framing was wrong. The runner's identical frames were likely the `--headless` gap below.

Harness gaps fixed: `--headless` with any scripted or frame-observing option (`6082945`);
`--dump-a11y` as an ordered, repeatable step, `--select` in the caret's document, and `[[` for a
literal leading bracket in `--type` (`e420bc5`). Found while fixing finding 5:
`DocumentSwitcher::describe()` pushed children through a node reference the next `add()` could leave
dangling (`5929b08`).

Since then: a tab names its store and branch rather than its version, which was the "1" to "3" seen
in J5 (`3d09f06`); `find` hits and saved result rows quote the bytes they came from, by address,
instead of copying them (`1b0f784`); the window icon moved into `assets/` (`d289ed9`). Still open:
the accessibility actions on pouch cards are untested against a real assistive technology, since
this build has no AccessKit.

## Not validated

- Dropping onto a second, different open page (J3): one document at a time was on screen, so only a
  drop elsewhere on the same page was driven.
- Multi-member endsets, several links over one span, and links into another store (J4).
- A selected link surviving relaunch, closing by a menu control, and reopening from inside the
  running program (J5).
- vquery over several stores at once, and a deliberately malformed store with several homes (J6).
- Vulkan and OpenGL ES; J7–J16.
