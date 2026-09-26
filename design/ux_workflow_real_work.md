# Real Work in Xuzz and VQuery: UX Validation Contract

## Purpose

A person should be able to open `xuzz` or `vquery` and do real work in them — write, gather,
arrange, navigate, stop and come back — without reading source, passing command-line flags or
editing files by hand. This document is the contract the
[`xuzz-ux-validation`](../.claude/skills/xuzz-ux-validation/SKILL.md) workflow validates against. It
describes journeys a user takes and what counts as done; it is not evidence that any of them works
today. Every validation run reports what the current build actually does.

A journey passes only when every step is reachable through the program's own interface:

- **Menus**: the radial menu, the command bar and palette, the pouch drawer, the link panel and the
  overview, or any other on-screen control.
- **Key bindings**: the defaults in `system://keymap`. An action that exists but has no default
  binding and no on-screen control is not reachable.
- **Pointer**: click, drag, drop and scroll on what is drawn.

Launching the program counts: `xuzz` with no arguments, or with a store path as a file manager would
pass it. Nothing else on the command line does. The automation options (`--click`, `--do`, and so
on) are the validator's hands, standing in for a person's; they are not part of the product.

## Journeys

Each journey lists its steps, the evidence a pass needs, and the failures to look for. "Evidence"
means artefacts a reviewer can inspect without rerunning: captured frames, the accessibility dump,
program output, and `xudu-dump` of the store afterwards.

### J1. Create a new xanadoc

1. Launch `xuzz` with no arguments.
1. Create a new xanadoc through a menu, then again through its key binding.
1. Type a few lines into it.

Evidence: frames after each step; the accessibility tree naming the new document; after closing, a
store directory holding the typed text (`xudu-dump --section=ops` with the permascroll).

Watch for: creation reachable only from the command line; a new document that opens but cannot take
the caret; typed text that does not survive a save.

### J2. Create a new slice

1. From the same session, create a new ZigZag slice through a menu and through its key binding.
1. Add a cell, give it text, and link it to another cell along a dimension, using the UI only.

Evidence: frames showing the slice and its cells; the accessibility tree's cell list; the store's
Structure operations (`xudu-dump --section=ops`).

Watch for: slices creatable only by `--structure-script`; cells that cannot be named or linked from
the keyboard; a slice that is not saved as a store.

### J3. Select, drag and drop

1. Type text into a xanadoc.
1. Select part of it with the mouse (press, drag, release).
1. Drag the selection into empty space: a new page (document) holding that text appears, and its
   content transcludes the original, sharing its primedia.
1. Select other text and drag it into a pouch zone. Close the pouch drawer.
1. Later in the session, open the pouch and use the stored item: drop it into a document, or forge a
   link from it.

Evidence: frames at press, mid-drag and after each drop; the pouch drawer's accessible items; the
store after closing, showing the transclusion (shared addresses, not copied text) and the pouch's
contents in `system://pouches`.

Watch for: a drop that copies text instead of transcluding it; a drop into empty space that does
nothing; pouch items lost when the drawer closes; no pointer feedback while dragging.

### J4. Home cell and back

1. From a xanadoc, go to the ZigZag home cell through the UI, and again through a key binding.
1. Return to the xanadoc the same two ways.
1. Repeat with the caret mid-document and with a selected link: the caret, selection and link
   context survive the round trip.

Evidence: frames on each side; the accessibility tree's focus after each move; the caret offset
reported before and after.

Watch for: a way there but no way back; the camera leaving the reader somewhere unreadable; the
caret reset to the document start; ZigZag focus that cannot be moved without the arrow keys the
document also uses.

### J5. Close, reopen, resume

1. With several documents open, a slice focused, the caret mid-sentence, items in a pouch and a link
   selected, close the store through the UI (menu and key binding).
1. Quit and relaunch, or reopen from inside the running program.
1. The documents reopen, the caret and camera are where they were, the slice focus and pouch items
   are back, and typing continues where it stopped.

Evidence: frames before closing and after reopening, compared; the accessibility tree's open
documents and caret; `xudu-dump` before and after, showing nothing was lost or duplicated.

Watch for: reopening to the default view; typed text that was never saved; a system xanadoc reset to
defaults; a session that cannot find its permascroll.

### J6. Query with vquery

1. Launch `vquery` on a store written in J1–J5.
1. Write a query that finds cells or text from the earlier journeys, read the result, refine the
   query, and save the result to a store.
1. Open that store in `xuzz`.

Evidence: the REPL transcript; the result store's `xudu-dump`; a frame of it in `xuzz`.

Watch for: errors that name internals instead of the query; results that cannot be saved; a saved
result `xuzz` cannot open.

## Findings

Each step ends in exactly one of:

| Outcome       | Meaning                                                                  |
| ------------- | ------------------------------------------------------------------------ |
| Pass          | Reached through the UI, with evidence                                    |
| Fail          | Reached, but the result is wrong; evidence of what happened instead      |
| No affordance | The program can do it, but only outside the UI (flag, script, file)      |
| Missing       | The program cannot do it at all                                          |
| Harness gap   | A person could do it, but the validator's automation cannot yet drive it |

A harness gap is a defect in the validator, not the product, and is fixed before the journey is
judged. The remaining outcomes are product findings, each with the step, its evidence and a proposed
fix.
