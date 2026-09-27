# Real Work in Xuzz, VQuery, VPL and VProlog: UX Validation Contract

## Purpose

A person should be able to open `xuzz`, `vquery`, `vpl` or `vprolog` and do real work in them —
write, gather, arrange, calculate, reason, navigate, stop and come back — without reading source,
passing command-line flags or editing files by hand. This document is the contract the
[`xuzz-ux-validation`](../.claude/skills/xuzz-ux-validation/SKILL.md) workflow validates against. It
describes journeys a user takes and what counts as done; it is not evidence that any of them works
today. Every validation run reports what the current build actually does.

A journey passes only when every step is reachable through the program's own interface:

- **Menus**: the radial menu, the command bar and palette, the pouch drawer, the link panel and the
  overview, or any other on-screen control.
- **Key bindings**: the defaults in `system://keymap`. An action that exists but has no default
  binding and no on-screen control is not reachable.
- **Pointer**: click, drag, drop and scroll on what is drawn.
- **REPL input**: expressions, facts, rules, queries and commands typed at a visible prompt.

Launching the program counts: no arguments, or a store path as a file manager would pass it. Nothing
else on the command line does. A source file prepared outside the program is not a substitute for
entering work in its interface. The automation options (`--click`, `--do`, and so on) are the
validator's hands, standing in for a person's; they are not part of the product.

## Journeys

Each journey lists its steps, the evidence a pass needs, and the failures to look for. "Evidence"
means artefacts a reviewer can inspect without rerunning: captured frames and accessibility dumps
for graphical steps, a terminal transcript for REPL steps, and `xudu-dump` of saved stores.

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

### J7. Total an editorial budget with VPL

1. In `xuzz`, make a slice for an issue budget with a rank of three numeric cells: writing 12,
   artwork 8 and rights 5. Save it and note which dimension holds the amounts.
1. Launch `vpl` and open that saved slice through its interactive interface. Inspect the dimension
   and the three amounts before calculating anything.
1. Use a VPL expression to total the amounts (25), then change artwork to 10 in the working view and
   recalculate (27). Inspect the view with `:grid` to check which cells contributed. Correct an
   expression after an error without losing the working view.

Evidence: the slice frame and accessible cells; a REPL transcript showing the inspected amounts,
both totals, the correction and the grid; the source store's operations before and after the
calculation. The numbers in the result must come from the slice, not a second hand-typed copy.

Watch for: store access available only through `--store`; arithmetic that silently works on a fresh,
unrelated arena; numbers rendered as text with no usable scalar value; a grid that cannot identify
the contributing cells; an error that destroys the working view.

### J8. Keep and reuse a VPL result

1. Continue J7 and save the revised budget as a new result slice through `vpl`'s interface, keeping
   the original budget available for comparison.
1. Open the result in `xuzz`. Find its amount cells and total, and compare them with the original.
1. Quit and reopen `vpl` on the result. Recalculate the total from the saved cells, without
   re-entering the numbers or using a source script.

Evidence: the REPL transcript showing the save and reopened calculation; `xudu-dump` of both stores,
including the result's Structure operations; a frame and accessible cells in `xuzz` showing the
saved values. The original and result must remain distinguishable.

Watch for: saving available only through `-o` or `--output-store`; an export that saves compiler
cells but not the calculated view; a result that `xuzz` cannot open; loss of the original data or
numeric type; a session that can calculate but cannot resume work.

### J9. Decide whether an issue can ship with VProlog

1. Launch `vprolog`. At its prompt, enter editing, rights clearance and approval facts for two
   issues. Leave approval absent for one issue; the other has all three. Enter a rule that an issue
   can ship only when all three are present. Use `listing.` to review the knowledge base.
1. Ask whether the unapproved issue can ship and read the negative answer. Ask which issues can ship
   and find only the approved one. Add the missing approval at the prompt and ask again; now both
   issues must appear as separate solutions.
1. Save the facts and rule through the interface, quit, reopen them in `vprolog`, and repeat the
   decision. The rule and evidence must survive without preparing a `.pl` file by hand.

Evidence: a complete REPL transcript with the facts, rule, listing, negative answer, added approval
and later solution; a second transcript after relaunch; the saved knowledge base and, where it is
represented as a store, its `xudu-dump`. The answer must change because the missing approval was
added, not because the rule was weakened.

Watch for: `assert(...)` accepting facts but losing them on exit; saving or consulting possible only
through a file edited outside the program; a rule that returns an incomplete or duplicate solution;
an error that leaks compiler or manifold internals instead of naming the failed query.

### J10. Switch between xanadocs, slices and both

1. Open `xuzz` with at least two xanadocs and a slice. Put the document caret mid-paragraph and
   focus a cell in the slice.
1. Choose **Xanadocs only** from the interface. Read and edit a xanadoc; the slice is absent from
   the view but remains in the store.
1. Choose **Slices only**. Move through cells and edit one; the xanadocs are absent from the view
   but remain in the store.
1. Choose **Both**. See the xanadoc and slice together, then switch among all three modes again.
   Return to the same document caret and cell focus without reopening either item.

Evidence: frames and accessibility dumps in each mode; the controls or bindings used to switch;
`xudu-dump` before and after, showing that filtering changed visibility rather than document or
slice content.

Watch for: a mode available only at launch; hidden items still intercepting pointer or keyboard
input; switching modes deleting, duplicating or silently closing content; the caret, cell focus or
camera reset on every switch; **Both** rendering only one type.

The eventual `xudu` and `zigzag` wrappers can select **Xanadocs only** and **Slices only** from this
same view choice.

### J11. Follow one link to two distant ZigZag cells

1. In **Both** mode, open a xanadoc and an extensive slice. Put cell A and cell B in different
   neighborhoods so neither is visible when the other is focused. Select a passage in the xanadoc
   and create one link whose document passage is one endset member and whose other endset has both
   cells' exact content spans. Inspect the link before navigating: it has one identity and two
   separately selectable cell members.
1. Choose cell A's member and enter it. Read the cell with its local neighborhood visible, then use
   Activity Back or Return to origin to restore the xanadoc passage and its link context.
1. Choose cell B's member of the same link and enter it even though B is outside the currently drawn
   neighborhood. Return to the xanadoc again. From the origin, use Activity Forward to choose either
   visit; both branches remain available. Save and reopen the session, then repeat the choice.

Evidence: frames and accessibility dumps showing the one link identity, both complete endsets, each
chosen cell and its neighborhood, the document origin and two activity branches; the stored link
record and the separate activity-store visits; the same endpoint identity and exact passage after
reopening. Compare the visited stores after link creation and after navigation: movement adds no
operations there. The detailed interaction contract is
[`ui_workflow_xuzz_navigation.md`](ui_workflow_xuzz_navigation.md).

Watch for: two separate links created instead of one many-to-many link; a rendered beam choosing an
arbitrary cell; cell B unreachable because it is outside the visible radius; a return that loses the
document caret or selected link; the second visit replacing the first branch; movement mutating the
visited store.

### J12. Transclude cell content into a xanadoc and another cell

1. Start in **Slices only** with a slice containing a cell whose content is a distinctive passage.
   Create a xanadoc through the interface and switch to **Both**.
1. Select the source cell's content and transclude it into the new xanadoc. Read the passage there
   and follow its provenance back to the source cell.
1. From the document or original cell, transclude that same content into a newly created cell in a
   different part of the slice. Edit surrounding document text and neighboring cell content, save,
   close and reopen all three locations.

Evidence: frames and accessibility dumps for the source cell, document passage and new cell;
`xudu-dump` showing their content spans address the same primedia and that edits around them have
their own operations; provenance and all three readable occurrences after reopening.

Watch for: retyping or copying bytes instead of transcluding; a document passage with no route back
to the source; a new cell that receives plain text but loses source identity; creating a xanadoc
forcing the slice to close; edits to surrounding content changing the quoted passage unexpectedly.

### J13. Turn VQuery data into a VProlog decision and a VPL report

1. In `xuzz`, make a store with three issue records. Two have editing, rights clearance and approval
   recorded; the third lacks rights clearance. In `vquery`, inspect the source records and produce a
   dataset of issue and gate facts that `vprolog` can open through its interface. Preserve which
   source cell each fact came from.
1. In `vprolog`, load that dataset through the REPL, enter a rule requiring all three gates, and ask
   which issues can ship. See two distinct ready issues and one unready issue. Save the query result
   in a form `vpl` can open through its interface, without retyping or hand-converting the rows.
1. In `vpl`, aggregate the result into a report: ready 2, unready 1, total 3. Save the report, open
   it in `xuzz`, and inspect the numbers and their route back through the Prolog result and VQuery
   dataset to the original issue cells. Reopen the report and recompute the counts.

Evidence: the VQuery, VProlog and VPL REPL transcripts; each saved intermediate and final result;
`xudu-dump` for store-backed stages; an `xuzz` frame and accessibility dump of the report; source
identities and the three expected counts after reopening. No stage may substitute a manually typed
copy for the preceding stage's result.

Watch for: a result that only the producing program understands; lost cell provenance between
stages; a Prolog query that returns only the first solution; a VPL total that counts duplicate or
missing rows; conversion requiring flags, external scripts or hand-edited files; a report that
cannot be reopened or audited.

### J14. Put every cell value kind on one rank

1. In `xuzz`, start a slice and build a `d.1` rank with six example cells in this order: ordinary
   text (`None`), integer `12` (`Int64`), floating value `3.5` (`Double`), `true` (`Bool`), an
   operation handle (`OpHandle`) and a reference to another slice (`ExternRef`). Inspect each cell's
   content and value kind before saving.
1. Pop up the OSMIC map, select a specific operation that minted a cell in the first slice, and
   annotate that operation with a short note. Place the resulting OpHandle cell on the rank. Read
   the annotation through the handle and confirm that it names the selected operation, not the
   current caret or an operation chosen by default.
1. Load a second slice with a distinctive cell, select that cell, and create the ExternRef from the
   first slice to its MakeCell operation. Place its persistent placeholder after the handle on
   `d.1`. Follow or inspect the reference to confirm it resolves to the second slice's cell without
   copying its content into the first slice.
1. Save, close and reopen both slices. Walk the six cells in order and inspect their text, typed
   values, handle target and foreign target again. Change the foreign cell's later state and confirm
   that the reference still names its original MakeCell operation.

Evidence: frames and accessibility descriptions of the whole rank, the OSMIC map with its chosen
operation and annotation, and both loaded slices; `xudu-dump` of the stores; a typed value query or
manifold inspection before and after reopening; the foreign target's stable microversion and the
first slice's unchanged primedia when the second slice changes.

Watch for: `12`, `3.5` or `true` remaining untyped text; a typed cell silently becoming plain text
after editing; an OpHandle that points to a later annotation operation instead of the chosen one; an
ExternRef that stores copied content or loses its scroll identity; a placeholder linked across
stores as a raw local cell index; or a rank that cannot hold all six kinds together.

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
