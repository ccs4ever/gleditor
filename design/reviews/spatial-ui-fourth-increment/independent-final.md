# Independent UX and visual review: bounded multiline landing

Reviewed the settled implementation from native captures and per-run logs. This is a bounded landing
review, not approval of J16, J24, J25, or a full reader journey. No product code was edited by this
reviewer.

## Result

The exact selected source now lands with a usable left gutter and stays clear of the Link dock. At
800×600, all eight highlighted source lines fit above the dock on OpenGL, OpenGL ES, and Vulkan. The
OpenGL and OpenGL ES PNGs are byte-identical; Vulkan has the same composition with minor raster
differences. The first glyph begins about 30 px from the left edge, so the previous clipping of the
initial `W` is resolved.

At 640×480, the layout makes an honest oversize landing: the beginning of the source is visible with
a 24 px left inset and roughly two full lines above the Link panel. The panel covers the selected
continuation, so the frame does not imply that the full selection fits. I did not exercise the
follow-up scroll in this capture. At 1440×1000, all eight selected lines and the destination passage
are visible together.

The page, selection, connector, and Link panel retain the existing palette and visual hierarchy. The
selected text is legible, and no selected line overlaps the Link panel in the 800 or 1440 captures.
At 640, the Link panel covers selected continuation lines; this is acceptable only as the prefix
fallback. The composition has generous dark negative space above the pages, consistent across the
sizes; I found no new visual element or backend-specific styling that needs a separate rule.

| Case              | Visual result                                               | State and persistence evidence                                                                                                               |
| ----------------- | ----------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| 800×600 OpenGL    | Full eight-line source; first glyph inset; text clears dock | `01-enter` reports Doc 2 and left member 1, source page 1/1 and bytes 0–654. Source and target operation dumps match their before snapshots. |
| 800×600 OpenGL ES | Same frame pixels as OpenGL; full range visible             | Same exact left range and source page; both source snapshots unchanged.                                                                      |
| 800×600 Vulkan    | Full eight-line source; same framing                        | Same exact left range and source page; both source snapshots unchanged.                                                                      |
| 640×480 OpenGL    | Beginning prefix readable; lower lines continue behind dock | Doc 2 / left member 1, source bytes 0–654; source and target snapshots unchanged.                                                            |
| 1440×1000 OpenGL  | Full source range and destination passage visible together  | Doc 2 / left member 1, source bytes 0–654; source and target snapshots unchanged.                                                            |

Evidence is under [`evidence/wide/final`](evidence/wide/final/). Each run retains its capture,
session/a11y log, arguments, activity dump, and before/after source snapshots. The native tree
confirms the selected document and exact source range, while the panel names the active left member.
Source and target operation snapshots are byte-identical before and after each run. These are
bounded interaction and persistence checks, not a claim that every visit-history transition was
audited end to end.

## Scope limits

The fixture exercises link creation, Enter, Origin, Back, and fold/unfold. It does not validate the
complete J16/J24/J25 journeys, an intermediate line wider than the terminal lines, long selections
across pages, unavailable or cancellation states, historical-version endpoints, or cell endpoints.
AT-SPI was unavailable; the retained native accessibility tree is not a platform accessibility
session. The UI-only enlarged-text setting was not reachable during the baseline affordance check,
so no enlarged-text capture is included. The 640×480 and 1440×1000 cases were run on OpenGL only;
the three-backend comparison is at 800×600.
