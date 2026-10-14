# Isolated-build 800×600 OpenGL check: failed

The isolated build completed the full scripted flow, but the first `01-enter.png` inspection failed
the visual gate: source ink is cropped at the left screen edge and lower selected text is covered by
the link panel. No other backend or size was run.

The action and retained range are coherent. The click on the visible Enter control was at
`(320,464)` and awaited `Doc 2=selected`; the subsequent a11y snapshot reports the left member
active, all eight source lines, source bytes `0–654`, and destination bytes `0–23`. Both source
stores are unchanged before/after. Origin, Back, fold/unfold, and activity capture completed. The
reported a11y caret is `77`; the diagnostic treats it as observed metadata and makes no
selection-corruption inference.

[camera-bounds-diagnostic.json](camera-bounds-diagnostic.json) records objective visible pixel
bounds. The source-side visible white pixels reach the left edge; the destination page occupies
x=538–799; a black band occupies y=136–276; and the link panel covers y=406–564. These are raster
bounds only. The native run did not emit world-space camera coordinates for `01-enter`; the final
saved activity place follows later Origin/Back/fold steps and cannot substitute for entry camera
state.
