# UX Audit 2026-10-02: Real Work on Main After the Xuzz Fold

The third run of the [`xuzz-ux-validation`](../.claude/skills/xuzz-ux-validation/SKILL.md) workflow
against [`ux_workflow_real_work.md`](ux_workflow_real_work.md), J1–J6, on `main` at `6965f93`: the
first build after xudu and zigzag were folded into one xuzz application, with view modes, scoped
documents and the Store Object Manager. Headless, OpenGL only. Five journey runners (J1+J2, J3, J4,
J5, J6) validated `6965f93`; the lead reran every disputed finding on the source and, after the
fixes below, on `5a3e943`. Evidence is under `/tmp/ux3-j12/`, `/tmp/ux3-j3/`, `/tmp/ux3-j4/`,
`/tmp/ux3-j5/` and `/tmp/ux3-j6/` on the machine that ran them. The previous run is
[`audit-2026-10-01-ux-real-work-rerun.md`](audit-2026-10-01-ux-real-work-rerun.md).

## Verdict

The journeys themselves hold: J1–J6 reach every step they reached before, stores persist without
loss, the activity store only grows, and a relaunch restores documents, caret, selection, slice
focus and pouch items. What broke was wiring the fold did not carry from `apps/xudu/main.cpp` into
`apps/xuzz/xuzz_app.cpp`, and it broke silently. All of it is restored (`a02a8a0`) and was checked
live after rebuilding:

| Lost in the fold                                                                                             | Seen in | Now                                                         |
| ------------------------------------------------------------------------------------------------------------ | ------- | ----------------------------------------------------------- |
| `--headless` returned before the renderer, dropping every scripted step                                      | all     | Runs the script and quits when it is done                   |
| Radial File > New xanadoc, New slice, Open did nothing (`run:` dropped)                                      | J1, J2  | Each runs its keymap action                                 |
| A pouch card's + did nothing (`setUseHandler` never called)                                                  | J3      | Transcludes the item at the caret (same span)               |
| Five overlays never constructed: drag ghost, page split, transcopyright, wireframe hull, collaborator carets | J3      | Constructed and registered; the ghost draws on canvas drags |

One defect predates the fold and was fixed with it: Ctrl+1–9 never switched documents, because the
keymap binds `std:xudu/doc_N` and the actions were registered as `doc-N`. One was found in this
branch's own code and fixed in `5a3e943`: vquery `--in-place` aborted (exit 134) on a store with a
second branch, promoting at `latest()` as rerun finding 1 once folded there.

The fold was found by comparing every handler, overlay and accessibility registration in the old
`main.cpp` with the new code; media widgets and accessibility sources carried over intact.

What still blocks or misleads, most severe first:

| #   | Finding                                                                                                         | Journeys             | Outcome       |
| --- | --------------------------------------------------------------------------------------------------------------- | -------------------- | ------------- |
| 1   | View modes (Unified, Xanadoc, ZigZag, cycle) have no key binding and no control                                 | J4                   | No affordance |
| 2   | A selected link does not come back after a relaunch; Alt+Shift+N reselects it                                   | J5                   | Fail          |
| 3   | The link panel is absent from the accessibility tree                                                            | J4                   | Missing       |
| 4   | After radial File > New xanadoc, typing lands in the previous document                                          | J1                   | Fail          |
| 5   | Store Object Manager "+ Slice" opens the new slice in two tabs                                                  | J2                   | Fail          |
| 6   | The Store Object Manager button's "=" is drawn at the top of the window; no binding                             | J1, J2               | Fail          |
| 7   | An untitled document's tab reads "Doc 2" live and "Untitled · a" after a relaunch                               | J5                   | Fail          |
| 8   | `find` searches only the documents a store designates current                                                   | J6                   | Partial       |
| 9   | `count(find("x")/d.source)` parses; `--engine vortex` refuses `find()`; the radial File sub-wheel is named File | `cf98bf9`, `a320f87` |               |

## Steps

| Journey | Step                                       | Affordance used                        | Outcome           | Evidence                                   |
| ------- | ------------------------------------------ | -------------------------------------- | ----------------- | ------------------------------------------ |
| J1      | New xanadoc                                | tab bar "+", Ctrl+N                    | Pass              | `/tmp/ux3-j12/j1/01_tabplus.png`           |
| J1      | New xanadoc by menu                        | radial File > New xanadoc              | Fail, fixed (4)   | `/tmp/ux3-j12/j1/03_radial_newdoc_*.png`   |
| J1      | Type, quit, kept and saved                 | keyboard, Ctrl+Q, `xudu-dump`          | Pass              | `/tmp/ux3-j12/j1/dump_untitled.txt`        |
| J2      | New slice                                  | Ctrl+Alt+N, Store Object Manager       | Pass (5)          | `/tmp/ux3-j12/j2/01_newslice.png`          |
| J2      | New slice by menu                          | radial File > New slice                | Fail, fixed       | `/tmp/ux3-j12/j2/radial_newslice_*`        |
| J2      | Edit, insert, mark, link; Structure stored | E, N, D, M, L; `xudu-dump`             | Pass              | `/tmp/ux3-j12/j2/dump_final.txt`           |
| J3      | Drag to empty space, to another page       | press inside selection, drag           | Pass              | `/tmp/ux3-j3/33_d1_afterdrop.png`, `43b_*` |
| J3      | Into a pouch; by key                       | drag; Ctrl+D, Ctrl+Shift+1..4          | Pass              | `/tmp/ux3-j3/21_pouch_fill.log`            |
| J3      | Pouch items survive relaunch; a11y tree    | F2, Ctrl+\\, `--dump-a11y`             | Pass              | `/tmp/ux3-j3/22_relaunch_check.log`        |
| J3      | Card's + inserts at caret                  | pointer                                | Fail, fixed       | `/tmp/ux3-j3/24_p_insert2.log`             |
| J3      | Drag ghost                                 | look at mid-drag frames                | Partial, fixed    | `/tmp/ux3-j3/32_d1_middrag.png`            |
| J4      | Home cell and back; state kept             | Alt+Home, F6, Escape, tile, page click | Pass              | `/tmp/ux3-j4/home/`                        |
| J4      | Caret keys; keys follow the focused pane   | arrows, Home/End, Ctrl+arrows; F6      | Pass              | `/tmp/ux3-j4/caret/`, `focus/`             |
| J4      | Link panel round trip                      | Ctrl+L … Alt+Shift+N/X/Return/O/B/D    | Pass (3)          | `/tmp/ux3-j4/link/`                        |
| J4      | View modes                                 | none found                             | No affordance (1) | `/tmp/ux3-j4/viewmode/`                    |
| J5      | Work, quit, relaunch with no store         | Ctrl+N, Ctrl+Alt+N, Ctrl+D, Ctrl+Q     | Pass              | `/tmp/ux3-j5/session1.log`, `session2.log` |
| J5      | Documents, caret, selection, slice, pouch  | relaunch                               | Pass              | `/tmp/ux3-j5/20_relaunch_initial.png`      |
| J5      | Stores intact, activity store only grows   | `xudu-dump` before and after           | Pass              | `/tmp/ux3-j5/pre_*.dump`, `post_*.dump`    |
| J5      | Selected link back                         | relaunch                               | Fail (2)          | `/tmp/ux3-j5/20_relaunch_initial.png`      |
| J5      | Tab titles stable                          | relaunch                               | Fail (7)          | `/tmp/ux3-j5/08_back_to_doc2.png`          |
| J6      | `##` on two branches; `find`; `contains`   | REPL, `-e`                             | Pass              | `/tmp/ux3-j6/01-hash-hash/`, `02-*`        |
| J6      | `:save`, `-o`: rows transclude             | REPL, `-o`, `xudu-dump`                | Pass              | `/tmp/ux3-j6/03-repl/result_find_ops.txt`  |
| J6      | `--in-place`, minting, two branches        | command line                           | Fail, fixed       | `/tmp/ux3-j6/05-inplace/`                  |
| J6      | Errors; two stores; result opens on slice  | command line, REPL, launch             | Pass              | `/tmp/ux3-j6/06-errors/`, `07-*`, `08-*`   |

Numbers in parentheses name the finding below.

## Findings

### 1. View modes cannot be reached

`std:xuzz/cycle_view_mode`, `view_unified`, `view_xanadoc` and `view_zigzag` are registered and work
under `--do`, but `defaultSettingSpecs()` gives none a chord and no control or menu entry names them
(`/tmp/ux3-j4/viewmode/`). Distinct from Ctrl+Alt+1–3, which filter documents and slices. Fix:
default chords in `apps/common/xanadu/system_docs.cpp`, and a named control beside the tab bar.

### 2. The selected link is not restored

The panel open before quitting is gone after a relaunch; the link and its spans are intact, and
Alt+Shift+N reopens the identical panel (`/tmp/ux3-j5/clean_relaunch_link_reselect.png`). J5's
contract includes the selected link. `LinkContext::restoreCurrentSelection()` is queued on resume in
`apps/xuzz/xuzz_app.cpp`; start there.

### 3. The link panel is invisible to accessibility

With the panel open, `--dump-a11y` names nothing of it: no link, sides, reading line, origin or
buttons (`/tmp/ux3-j4/link/sequence.log`). The pouch drawer had the same gap. Fix: make
`apps/xudu/link_panel_overlay.cpp` an `a11y::Source` with a group, its lines and named buttons.

### 4. Radial New xanadoc leaves the keyboard behind

After File > New xanadoc the new tab is selected, but typed text goes into the previous document;
after Ctrl+N it goes into the new one. Reproduced by the lead on `5a3e943`. The radial runs the
command from the render thread inside a pick, Ctrl+N from the event thread; the selection of the new
document likely loses to the click's own handling. Fix in `apps/xuzz/xuzz_app.cpp`'s radial handler,
for instance by queueing the command rather than running it inside the pick.

### 5. "+ Slice" opens two tabs

`StoreObjectManager::createSlice()` calls `onCreate_`, which opens the slice, then refreshes and
toggles the same item open again (`apps/common/ui/store_object_manager.cpp` ~71–87). Fix: toggle
only when no `onCreate_` opened it.

### 6. The Store Object Manager button

Its "=" is drawn at `height - 7` instead of `top - 7` (`src/doc_switcher.cpp` ~165), so it lands in
the window's top-left corner and the button is a blank rectangle. It also has no default chord,
against the rule that every UI action ships with one.

### 7. An untitled tab changes name across a relaunch

Live, the second document reads "Doc 2"; relaunched, "Untitled · a". A document's name is taken when
it loads, and by the relaunch its version was on a branch, so the branch suffix applied with
"Untitled" for want of a store name. Fix in `Session::sourceFor` (`apps/xudu/session.cpp`): no
branch suffix for an untitled store, whose tabs the bar already numbers.

### 8. `find` reads only the current documents

On `core_hypertext/unified_store`, which designates only `a3` current, `find("Hypertext")` found the
slice's cell but not the other document's prose that holds the word. J6 read the empty
`find(...)/d.source` that followed as a broken link; it was a slice cell, which has no `d.source`.
The link from a prose hit is reachable by query (`5a3e943` pins it). Whether to search every branch
head, not just the designated ones, is a decision: it finds more, and finds forks of one line more
than once.

### 9. Smaller findings

- The radial File submenu is announced as "Alignment Sub-Menu".
- F2 is the default for both the pouch drawer and the command bar.
- `count(find("x")/d.source)` is a parse error: `count` takes a path, not a path starting with a
  function.
- `--engine vortex` has no `find`.
- The J3 runner found and killed an orphaned `build/xuzz` from 2026-09-30 that had run for 25 hours
  at about 1340% CPU, on a script ending `--right-click … --wheel 0,0 … --chord Alt+/`. A livelock
  in that sequence is possible; not reproduced.

## Not defects

- **J5's Ctrl+2 switched a ZigZag bundle.** After Ctrl+Alt+N, ZigZag has the keyboard, and Ctrl+1–5
  there are the bundle keys by design (`keymapScope()` puts `std:ui` actions in the ZigZag pane). In
  the document pane they switch documents.
- **J5's silent Ctrl+D after making a slice.** The keyboard was still in ZigZag; `--select` sets a
  selection without moving the keyboard, which a person's press and drag would (harness gap below).

## Harness gaps

- `--select` does not move the keyboard to the document it selects in.
- The keymap's log category is `xuzz.keymap` since the fold; the skill said `xudu.keymap` (fixed).
- `--capture` does not wait for a settled frame, so a pouch count can read stale in a capture that
  the next `--dump-a11y` reports correctly. Documented behaviour, easy to misread.
- No REPL driver in `tools/`: J6 wrote its own PTY driver again.
- The build has no AccessKit; accessibility evidence is the harness's own dump.

## Not validated

- Whether a panned camera is reapplied on relaunch, as opposed to stored: a tab click refits the
  view either way, and the frame after relaunch was of a document that had not been panned.
- Multi-member endsets, several links over one span, links into another store.
- Closing through a menu control, reopening from inside the program.
- Vulkan and OpenGL ES; J7–J16.

## After the run

Every finding above is fixed on `feature/xuzz-link-context`; each was rechecked headless on a copy
of the fixture it was found on, and all four suites pass (gleditor 538, xudu 1203, xuzz 56, zigzag
120).

| #   | Fix                                                                                                                    | Commit    |
| --- | ---------------------------------------------------------------------------------------------------------------------- | --------- |
| 1   | Ctrl+Alt+V cycles view modes; Ctrl+Alt+Shift+U/X/Z pick one; radial View sub-wheel                                     | `a320f87` |
| 2   | The reading place keeps the selected link along `d.selected-link`; a resumed session reselects it                      | `ee5382a` |
| 3   | The link panel is an accessibility source: a "Selected link" group with its lines and buttons                          | `25e56a5` |
| 4   | A harness defect, not the program's: a `--click` step let the next step run before the click's queued work             | `9b64353` |
| 5   | "+ Slice" opens the new slice once                                                                                     | `a320f87` |
| 6   | The Store Object Manager's glyph is drawn on its button; Ctrl+Alt+M opens it                                           | `a320f87` |
| 7   | An untitled document's tab has no branch suffix                                                                        | `bc291c1` |
| 8   | `find` reads every branch head, designated ones first, and reports a line once by its spans                            | `88a8b2b` |
| 9   | `count(find("x")/d.source)` parses; `--engine vortex` refuses `find()` by name; the radial File sub-wheel is announced | `cf98bf9` |

Fixing 2 turned up two defects the run had not seen:

- **Links resolved only in store 0.** A resumed session reopens its stores as auxiliaries of the
  default one, so a link in any of them was "not found". The resolver now looks in every open store
  (`ee5382a`).
- **A headless session saved a phantom document.** The batch orchestrator's view registrations
  survived into the UI, so every `--headless` session recorded its document twice and came back with
  a second tab of the same version. They are cleared before the renderer opens its own (`ee5382a`).
  Earlier relaunch evidence from headless runs is affected: a duplicated tab there was this, not the
  program.

Finding 4 reproduced only under the harness. A person types long after the queued open and caret
move have run; a scripted `--type` straight after `--click` ran in the same frame, on a "settled"
judged before the click's handler queued them. Ctrl+N passed because a chord is an input step, which
already waited.

The harness gaps are closed too: `--select` moves the keyboard to the documents (`1c832e0`), and
`tools/repl-transcript.py` drives a REPL through a PTY for the terminal journeys (`9ed4230`). F2 and
Ctrl+1–5 stay shared between a document action and a ZigZag action by scope, as intended. The
orphaned `xuzz` livelock was not reproduced.
