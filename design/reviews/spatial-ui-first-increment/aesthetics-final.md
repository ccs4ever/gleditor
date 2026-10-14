# Independent native aesthetics review: first spatial increment

Reviewed 2026-10-14 as a separate visual pass by a reviewer who wrote no product code. The review
applies the Xuzz UI design skill, initial aesthetics notes and the existing theme language to actual
application frames. It is a bounded prerequisite review, not visual completion of the spatial
design.

## Inspected frames

Native sources: `evidence/luna-1/{quiet,expanded,folded}.png`,
`evidence/luna-1/keyboard-enter/enter.png`,
`evidence/luna-1/preview/{expanded,preview-left-occ2}.png`, and
`evidence/luna-3/{investigation-wide,investigation-page2}.png`. The earlier primary expanded frames
precede the complete-control correction and are not final evidence.

The final matched `evidence/luna-1/anchor-final/{quiet,expanded,folded}.png` captures were also
inspected directly from their original PPMs. The focused Harbor passage and page edge stay at the
same position through inspect/fold; the header is intact. This resolves the bounded same-document
composition question that the original blank authoring frames could not answer. The original PPMs
have identical pixels in the right source region `(703,477)-(1440,964)` across quiet, expanded and
folded. This supports visible anchor stability without claiming an unmeasured camera or
distant-endpoint contract.

Backend sources inspected directly:
`/workspace/spatial-build/backend-final/{opengl,opengles,vulkan}.ppm`, OpenGL/OpenGLES grown frames
and the Vulkan no-culling frame. These are the actual editor backend comparison captures; all draw
legible toolbar/document text with coherent placement. They contain no Link investigation panel, so
they establish the baseline drawing comparison only. They do not prove the new inspector's
three-backend parity or enlarged-text behavior.

The subsequent real UI-authored inspector captures were inspected directly:
`evidence/inspector-opengl/{quiet,expanded,preview}.ppm`,
`evidence/inspector-opengles/{expanded,preview}.ppm`, and
`evidence/inspector-vulkan/{expanded,preview,folded}.ppm`. OpenGL and OpenGLES expanded frames show
matching panel bounds, complete five Preview controls and clean identity text. OpenGLES preview also
keeps its full header and exact metadata. An early read of Vulkan's expanded file showed missing
header fragments while the capture process was active; reinspection after completion shows its full
header, both side counts, complete five controls and exact metadata intact. The finalized Vulkan
preview/folded frames are also legible. The finalized set passes bounded inspector composition
comparison across the three backends. These authoring-state frames lack visible prose, so they are
inspector evidence rather than a second anchored-reading proof.

## What works visually

The expanded native tablet belongs to the existing dark chrome family: slate surface, pale body
text, muted unavailable controls and cyan active-side emphasis. The active side also has a triangle
and a Left/Right label, so color is not its only cue. Its crisp rectangular content/metadata
treatment does not pretend that tool chrome is a stored page. Authority, current reading and Return
checkpoint are aligned, readable and separate; the inspector does not invent a brighter decorative
link hue. The restrained surface is appropriate for a prerequisite interface before physical tablet
wings.

In `luna-1/expanded.png`, the two-left/three-right Preview controls form complete rows with no
missing third column. The footer keeps Fold investigation distinct from paging and dismissal. Long
exact identity remains ordinary text rather than being reduced to a decorative badge. The broad
canvas gaps and low ornament avoid competing with the source's link ribbons.

`luna-3/investigation-wide.png` shows an honest composition limit: a readable page occupies the
available space, so expansion reports its lack of canvas rather than drawing a full-screen tool over
the prose. This is a useful fallback, though it does not deliver the intended reading-plus-tools
composition at that state.

## Unresolved visual findings

1. The original authoring frames contain blank page regions and no readable prose. The final matched
   sequence establishes stable focused-passage bounds after explicit entry and return, so bounded
   fold/expand continuity is now evidenced. Full page/cell continuity under different navigation and
   density remains open; the original blank state is not proven to be an increment regression.
1. The panel dominates a substantial area of the original expanded frame, while the source has no
   readable focal point. The intended hierarchy is content first, tool second. Judge the final
   proportion against a readable source state before investing in silver rims or glass optics.
1. In the expanded control grid, button text sits close to the upper/left edges, unlike the more
   generous inset of the quiet panel's controls. Reuse the shared button content inset consistently;
   do not fix it with a screenshot-specific offset. This is a polish finding, not evidence of an
   unreachable command.
1. An early read of the active Vulkan expanded capture appeared damaged; the finalized file is
   intact. That observation is not evidence of a product rendering regression. The older
   `luna-1/preview/expanded.png` also appeared clipped when inspected; use finalized clean matched
   captures for acceptance and investigate any persistent clipping only after capture completion.
1. No inspected capture establishes numeric contrast at every text opacity, reduced-motion cuts,
   pointer/keyboard focus appearance, hover/pressed treatment, or investigation rendering at
   multiple UI scales. Those gates stay open despite the bounded three-backend composition
   comparison.

## Disposition

The native increment is visually coherent enough to review as a tested, opt-in service and retained
panel checkpoint. It is not yet a polished spatial workspace or a visual cutover candidate. Preserve
the calm theme-derived surface; the next visual proof should extend the demonstrated same-document
source stability to complete reachable lists under density and scale pressure. L6/L7/U4a and the
view-host/camera gates still precede physical depth, wings and movement. This review does not accept
any complete J17-J32 journey.
