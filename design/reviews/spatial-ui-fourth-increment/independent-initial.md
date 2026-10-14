# Independent initial review: bounded multiline landing

Reviewed native HEAD `c1ab2314` using `xuzz-ui-design` and `ux-validation`. This reviewer wrote no
product code. This checkpoint extends the existing document landing correction; it does not
authorize physical wings, a new view host or a parallel camera architecture.

## Actual native finding

In `evidence/wide/baseline/640x480/01-enter.png`, the reader enters the UI-authored multiline left
endpoint. Doc 2 is correctly selected. The source begins at approximately x62, y240. Its first line
runs beyond the right viewport edge; subsequent selected lines pass behind the lower Link dock
(starting around y286) and the overview (around y261). The dark dock has clear labels and restrained
colour, but its composition hides the passage being entered. This is an actual narrow-window
readability failure, not a hypothetical inference from the code or a failure of tab synchronization.

At `evidence/wide/baseline/800x600/01-enter.png`, the first five selected lines fit, but the
overview hides the beginning of the following lines and the lower dock hides the final line. This
separates a vertical chrome/placement failure from the additional horizontal overflow at 640.

The corresponding `evidence/wide/baseline/1440x1000/01-enter.png` shows all eight selected lines
(bytes 0–654) readable above the Link panel. The proven failure is the narrow viewport, not every
multiline landing.

The previous three-backend short fixture remains useful: its selected single-line passage is visible
above the panel. It does not establish complete multiline or long-range visibility. No whole J16,
J24 or J25 acceptance follows from either fixture.

## Bounded implementation recommendation

Reuse retained page layout and the existing `Views` semantic entry boundary. `Page::textBandFor`
already supplies exact intersecting vertical line reach without reshaping. `clusterBoxes()` retains
uploaded quad centres and dimensions; `highlightFor()` computes intersecting cluster fractions.
Expose reusable range bounds from those retained structures, preserving the full line-height reach
and any intermediate wider lines. `rangeTerminal()` is a contour-routing query, not a complete-range
bounding box: its first/last terminals must not substitute for intermediate range geometry. Its
`singleQuadClusters` refusal also demonstrates a case that must be handled explicitly rather than
silently treating incomplete glyph geometry as complete.

Transform page-local bounds through existing page frames. Wait for the required layout rather than
claiming complete bounds from only first/last anchors when intermediate pages are absent. The
current Link pending-entry path requests start and end offsets; a full multipage-fit claim requires
a separate readiness contract. Do not add shaping, blocking content resolution or per-frame
allocating geometry work to the hot path.

Use live and settled chrome insets to define the available reading rectangle. Keep the configured
readable text floor. Fit the complete selected range when it fits at that floor; when it cannot fit,
prioritize the exact beginning with usable line height and continued ordinary scrolling. Do not
shrink text until the full pair or arbitrarily long selection fits. Preserve source-page pose and
show its companion where space permits. Place the existing Link panel around protected selected
source geometry; a dock must reserve space before the semantic landing is considered settled.

This is ephemeral camera/layout work. Preview, Cross, inspect and fold must keep focus and completed
visit count unchanged. Enter creates one successful visit; Return and Back restore existing nodes.
No source operations, occurrence substitutions or latest-version fallback are acceptable.

## Final review gates

Inspect new native frames for multiline ranges, an intermediate line wider than both terminal lines,
long oversize selections, tighter windows and enlarged text. Include all three compiled backends and
match selected tab, exact caret/range, Link identity, active side and visit to the transcript/tree.
Compare same-profile source and activity snapshots. Inspect the reading beginning against all
visible chrome, and distinguish complete-fit from an honest readable oversize fallback. Review
pending and unavailable/cancel behavior separately. Retain the existing AT-SPI, optimized
performance, historical and cell endpoint gates; no diagnostic tree dump constitutes a platform
accessibility session.
