# Independent final review: sixth spatial UI increment

The frozen candidate fixes the fifth-increment failure in the tested bounded route: on Vulkan,
visible Enter enters the selected source occurrence, ordinary wheel input reaches text on the later
physical page, and the Link selection stays active during that continuation. This review covers a
normal 7,439-byte source and a forced-break 7,438-byte source, plus one UI-scale replay. It does not
certify pending-entry cancellation or the full J24/J1–J32 journeys.

## Journey evidence

The candidate pair was `xuzz` SHA-256
`404c21bc477c8dd5324bbba7957545f9de50ba52b0f96f38b94dfb3bd68a6cbd` and `libgleditor.so.0` SHA-256
`6964780c202365c8da5ada84fad51f0d21fc621a0c2db72ad0ebc792b75ffcf6`. The Vulkan runs used the
prepared native-session container, fresh XDG profiles, X11/Xvfb with `-noreset`, the default keymap,
and the visible Enter button. UI text was typed through the normal interface; there was no `--do`,
model, path, or file-content injection. `argv.txt`, the session/action log, internal accessibility
dumps, source/activity snapshots, exit status, frame hashes, and PNG provenance are retained per run
under [evidence/vulkan](evidence/vulkan/).

| Vulkan run                                                 | Authored source                                                                                                                              | Entry and continuation                                                                                                                                                                                                                    | Return                                                                                                                                                        |
| ---------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [normal](evidence/vulkan/normal/)                          | 7,439 bytes, 80 typed lines, no forced break; exact source selection `[0,7439)`, target `[0,23)`                                             | `Doc 2` is selected after Enter. The chosen source occurrence reports layout pages 1–2/2. Ten single-tick ordinary wheel inputs retain the selected range and Link identity; at tick 6 the capture shows P062 on the later physical page. | Origin selects `Doc 3` and focuses the `home` cell. The following Back capture is identical, so this run does not show Back restoring a prior visit.          |
| [forced](evidence/vulkan/forced/)                          | 7,438 bytes, 80 typed lines; real `Ctrl+Return` at byte 3,719; exact source selection `[0,7438)`, target `[0,23)`                            | `Untitled · a` is selected after Enter. Its chosen occurrence reports pages 1–2/2 and “continues across page breaks.” The finer wheel sequence reaches P041 on page two at tick 6 while keeping the link and highlight.                   | This first run recorded Origin then Back; both later frames are retained, but this order does not prove Back restoration.                                     |
| [forced, UI scale 1.5](evidence/vulkan/forced-scale15-ui/) | Same forced fixture and exact range as above; the native `Ctrl+Alt+Shift+F` form was opened, `1.5` typed, and Enter pressed before authoring | Entry and page-two continuation are unchanged. The form states that the scale applies to application chrome typography and that document text size is separate.                                                                           | Back is invoked before Origin. Back returns to the `default` document with the source link selected; Origin then selects `Doc 3` and focuses the `home` cell. |

The forced-scale run is the correctly calibrated scale replay: its `argv.txt` records
`ui_font_scale=1.5`, and its log contains the native scale form and subsequent interaction. An
earlier run directory was initially named `forced-scale15` even though the variable was outside the
container boundary and the run stayed at default scale; it was renamed
[forced-default-back-origin](evidence/vulkan/forced-default-back-origin/) and is retained as a
default-scale diagnostic. It is not counted as scaled evidence.

The separate [GLES forced supplement](evidence/gles/supplement-preenter/) adds a 20-second hold
after link preview and before the visible Enter click. Its profile snapshot shows one visit record
with current index 1 before Enter, then two records with current index 2 immediately after and at
exit; the selected source row reports pages 1–2/2, the P041 continuation is visible after ordinary
wheel input, and source operation dumps remain byte-identical at before/enter/after. Its per-frame
PNG round-trip check reports zero changed pixels against each captured raw PPM. This strengthens the
successful single-Enter and physical-page continuation evidence across GLES, but the hold happens
before Enter and does not exercise a delayed pending entry.

The clearest Vulkan frames are the [forced landing](evidence/vulkan/forced/03-enter-selected.png),
[forced continuation onto page two](evidence/vulkan/forced/04-wheel-06.png),
[Back after the single Enter visit](evidence/vulkan/forced-scale15-ui/05-back.png), and the
subsequent [Origin result](evidence/vulkan/forced-scale15-ui/06-origin.png). The higher-resolution
GL comparison is visible in its
[1440×1000 forced continuation frame](evidence/gl/candidate-1440-forced/04-continuation-wheel.png);
GLES examples are in [normal](evidence/gles/normal/10-continuation-wheel.png) and
[forced](evidence/gles/forced/10-continuation-wheel.png) continuation captures.

Source operation dumps from before entry and after each Vulkan journey compare byte-for-byte. The
session logs show the exact selected range, ready endpoint anchors, the first-caret fallback for the
cross-page range, and `reading: left member 1` during continuation. The default-scale forced run's
activity snapshots show one record before Enter and two at the immediate post-entry and final
snapshots, consistent with one endpoint visit being added and no further visit record during the
rest of that replay. The normal and 1.5-scale forced runs already contain two records in their
pre-entry snapshots, so their activity counts cannot isolate Enter's contribution. Their
ordinary-wheel frames and accessibility state still show the chosen source and link remaining active
while the reader continues.

Entry settled quickly in these fresh profiles. They therefore show the successful ready-anchor path,
but they do not exercise a delayed pending-entry frame. I did not observe or test cancellation of a
genuinely pending entry, so the requirement that a cancelled or replaced pending request can never
travel later remains open. The internal `--dump-a11y` tree is available; platform AT-SPI was not.
The `place 1/1` panel labels are occurrence counts, while `pages 1–2/2` is layout metadata.

## Visual and interaction assessment

At 800×600, the initial fit places the first selected lines above the Link panel. The cyan selection
is legible against the white page, the existing dark-blue/cyan panel hierarchy remains consistent,
and the gold focus marker stays distinct. The Link panel does cover a substantial lower portion of
the narrow viewport. Ordinary wheel continuation exposes the page gap and later selected text; the
forced fixture clearly resumes at P041. A long 10-tick probe eventually reaches mostly empty page
space, so users need a moderate wheel gesture. The dedicated `04-wheel-06.png` frames are the
clearest page-continuation evidence.

At 1,440×1,000, the independent OpenGL captures place the Link panel alongside the page, and
substantially more of the selected passage remains readable at once. The OpenGL 800×600 and
1,440×1,000 normal/forced matrix, each with the same governed 1.5 chrome scale, is documented in
[the GL journey report](evidence/gl/report.md). The separate GLES 800×600 normal and forced journeys
also show the selected source continuing across pages and retain the link panel. I inspected their
captured frames alongside Vulkan. At these sizes, all three backends preserve the same selection and
panel hierarchy; I saw no user-visible backend difference that changes the journey or readability
conclusion. Vulkan lossless PNGs were produced by copying each raw P6 PPM RGB frame unchanged;
`frame-provenance.txt` records the RGB, PPM, and PNG SHA-256 hashes. Raw Vulkan PPM files and
private XDG profiles remain outside Git at `/workspace/spatial-build/sixth-VKscratch/raw/` and the
run-specific container profile paths.

The 1.5 setting is explicitly a chrome-text scale control. It does not enlarge the document glyphs,
and this replay does not establish a document-text accessibility-size control. Larger viewports
improve the page/panel balance; the narrow viewport still has a dense dock that competes for
vertical reading space. The separate strict core pixel comparison remains open. The current capture
audit records differences within document glyph bounds 173309–554424, while the toast panels at
y=482–572 match. Baseline-control Vulkan thread captures match; the GL/GLES overlay difference
hashes also match the frozen candidate. No cause is assigned here, and the open pixel gate prevents
a finished-gate claim.

## Scope and evidence limits

This is a focused multipage Enter-and-continue regression review, with return controls observed in
the runs noted above. It is not acceptance of the full J24 workflow or all J1–J32 journeys. It does
not claim pending cancellation/stale-request validation, platform AT-SPI coverage, optimized
performance, tablet behavior, or a new camera/view-host architecture. The candidate build is O0-g0,
but these UI runs are not performance acceptance. The camera and media/tablet architecture items
remain deferred to L6/L7/U4a work. No performance conclusion is drawn from these native UI runs.
