# Independent native UX review: first spatial increment

Reviewed 2026-10-14 against the working implementation on `feat/spatial-ui-overhaul`, based on
`e9726a6b`. This reviewer wrote no product code. This pass applies the Xuzz UI design skill and the
initial UX findings to actual native captures, input transcripts and the product diff. It does not
turn replay exit codes or prototype states into journey passes.

## Bounded disposition

The native prerequisite increment is reviewable: it extends the existing Link navigation and
retained panel rather than introducing another camera, view host or Link identity. The inspected
2-by-3 native Link exposes both complete member sets, names preview actions separately from Enter,
and displays exact reading/checkpoint metadata. D1 and the selection return checkpoint have useful
owning tests. Full J22/J24/J25 acceptance and J1-J16 preservation remain gated by the validation
record; this review supplies no blanket acceptance.

## Actual evidence inspected

- `evidence/luna-1/{quiet,expanded,folded}.png`: Link 22, complete two-left/three-right counts, both
  cursor summaries, visible Inspect members and Fold investigation controls. The expanded frame
  shows all five Preview member controls. This is actual native authoring evidence.
- `evidence/luna-1/preview/stdout.log` and `preview/preview-left-occ2.png`: real click-label
  delivery to Inspect members, Preview Left 1/2 and Preview Left place 2/2; names and selected
  states in the native accessibility dump. The full description distinguishes current state `a34`
  from the chosen quoted occurrence state `b1`, source scroll byte address and occurrence-local
  range. It retains visit 1 while browsing. Accessibility dumps are not an AT-SPI session.
- `evidence/luna-1/keyboard-enter/enter.png`: an explicitly entered endpoint shows readable Harbor,
  Meadow and the three right passages, a chosen left occurrence and separate Enter, Origin and Back
  controls.
- `evidence/luna-3/investigation-wide.png`: readable document at 1920 by 1200; expansion refuses
  insufficient free canvas, preserves a compact selected-Link panel and displays the reason.
- Final `evidence/luna-1/anchor-final/{quiet,expanded,folded}.png` and its transcript: after
  choosing and entering the first left endpoint, then returning to origin, the visible focused
  Harbor passage stays at the same position through pointer Inspect members and Fold investigation.
  The dump keeps document focus, caret 20 from 0, state `a34`, bytes 0-20 and visit 1; the expanded
  header is clean. This passes the bounded same-document fold/expand continuity inspection. It does
  not prove a distant endpoint, branch restoration or full page readability under every ribbon
  arrangement. Pixel comparison of the original PPMs confirms the right source region
  `(703,477)-(1440,964)` is identical between quiet and expanded, and between quiet and folded; this
  is raster evidence, not an assertion about unrecorded camera transforms.
- Product diff in `LinkContext`, `LinkNavigator`, `StoreActivityLog`, `ContentReference`, panel
  presentation/overlay, system keymap and app registration; owning selection/activity, persistence
  and sixty-control prepared-layout tests were read. Test execution results belong to the lead's
  gate record.

The old `evidence/primary` captures precede the fractional-width control correction and are excluded
from complete-control acceptance.

## Findings and gates

| Priority         | User action and observed result                                                                                                                                                                           | Required next proof or correction                                                                                                                                                                                                                  |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Resolved subset  | Inspect the original Link 22 pair: prose was absent. The final matched Link 21 sequence starts from a readable focused endpoint and retains its position, focus and exact selection through inspect/fold. | Same-document anchor continuity is evidenced. Keep the original limitation visible; repeat for distant page/cell endpoints, scale and density before broader acceptance.                                                                           |
| High             | Select and preview members: transcript retains visit 1 and provides explicit preview controls; source/activity separation is addressed in the diff and owning tests.                                      | Retain before/after source and activity records proving no completed visits or visited-source writes for selection, browsing, inspect and fold. Enter must append exactly one visit; Return/Back/reopen must restore their distinct exact targets. |
| High             | Try the expanded tool over the large readable document: native UI reports insufficient free canvas and falls back to Controls.                                                                            | Record this as explicitly unavailable expansion, not a completed investigation workspace. Exercise recovery and enlarge-text/narrow-window paths; preserve reachable Dismiss, Enter, Return and exact retained selection.                          |
| Medium           | Preview a repeated occurrence: its exact state/address is available in the accessible full description, while the visible page is 1 of 5.                                                                 | Exercise all pages and inspect the chosen occurrence's exact visible metadata and each reachable control. A count is not proof that every row was visited.                                                                                         |
| Evidence caution | An early read of the active Vulkan expanded capture showed missing header fragments. Reinspection after process completion shows a clean full header and all controls.                                    | The incomplete-read observation is not a product failure. The finalized OpenGL/OpenGLES/Vulkan captures pass bounded inspector composition review; scale and motion remain unpassed.                                                               |
| Gate             | Pointer click-label and default keymap routes are present; this build has no AccessKit integration.                                                                                                       | Full pointer/keymap/assistive-technology identity parity remains unpassed. Mark the AT runtime/build limitation separately from native behavior.                                                                                                   |

The command boundary remains coherent: SelectMember/SelectOccurrence/Cross preview; Enter travels;
the new `Ctrl+Alt+I` command changes presentation only. Exact references explicitly refuse partial
occurrences without offset mapping, package presentation spans and ambiguous document frames rather
than substituting a nearby state. The frozen checkpoint is separate from the old visit's target;
Back and Return can consequently restore different valid locations. Old activity payloads are
preserved and refused, which must be stated in the continuation checkpoint rather than hidden as
successful reopen compatibility.

## Remaining acceptance scope

Actual dense/endset and unavailable/cancel runs, sibling futures, native reopen, narrow/enlarged
text, reduced motion, AT input parity and measurements still require their recorded evidence.
Search/comparison/layers, acquisition policy, typed durable cards, media clocks, physical wings and
view-host milestones remain dependency-backed later work. The three Luna reports must retain their
individual native findings for J17-J32 and J1-J16; this review does not override their failures or
missing-affordance results.
