# Independent initial review: fifth spatial UI increment

Reviewed the current native implementation and archived fourth-increment evidence using
`xuzz-ui-design` and `ux-validation`. This review makes no product-code changes and accepts no
complete J1–J16, J24 or J25 journey. The fifth increment should stay within existing document,
selection and camera seams; physical tablets, a new view host and new camera architecture remain
gated elsewhere.

## What is wired now

The exact Link Enter handler in `apps/xuzz/xuzz_app.cpp` enters the chosen source range, preserves
its selection, aligns the source and companion, then calls `Views::keepRangeInView()` after Link and
camera motion settles. `apps/common/ui/xanadoc/views.cpp` computes exact retained text bounds only
when both endpoints lie on the same built page and `Page::textBoundsFor()` can describe the range.
That path includes chrome insets, the overview rectangle, the safe margin and a readable caret
fallback. For ranges crossing pages, or unsupported/oversized single-page ranges, it deliberately
uses the first caret rectangle. It does not claim the whole range fits.

Ordinary reading movement is already available through the shared camera. `src/app.cpp` maps
unmodified wheel input to scene pan, Ctrl-wheel to zoom, Shift-wheel to horizontal pan, one-finger
touch to pan and the existing camera chords to movement. These paths update `state->view.pos`
directly. `Views::keepInView()` is the typing/caret-follow path: it keeps a point within an 80%
camera comfort rectangle, but does not reserve chrome or the overview. There is no separate camera
service in this path. A multipage continuation should preserve the exact selection while ordinary
camera movement reveals later built pages; it should not repeatedly refit the full selection or
steal the camera back to the first anchor.

The fourth-increment final evidence shows the intended entry composition at 800×600 and 1440×1000. I
inspected `evidence/wide/final/800,600-opengl/01-enter.png` (SHA-256
`f7e397d6380de4ae3d926c8b4f995f2bf801aee906b23ed4547c30cf4db3c506`) and the indexed 1440×1000
capture (PNG SHA-256 `42db1bf8cc524dc7b35255bb9d682f11d4062fa4fb0d99a8c25bf3d532467fcc`). At
800×600, the selected eight-line passage is readable above the Link panel, with a clear left inset;
at 1440×1000, the companion passage also fits. The validation report records same-profile source
dumps unchanged, three backends for 800×600, and only a bounded landing replay. These are sound
visual anchors, not multipage evidence. For provenance, the archived 800×600 and 1440×1000 OpenGL
PPMs match their checked-in PNGs pixel-for-pixel (ImageMagick absolute-error count 0); their PPM
hashes are listed in `evidence/wide/raw-frame-and-profile-manifest.md`.

The archived `font-setting-affordance/ui-document.png` (SHA-256
`06624dd34df12f3eb3c280c878a1f8efb8b63587e857ae4a8fd16971214e779b`) shows the UI system document
with its schema text selected; `open-palette.png` (SHA-256
`7286b653eaf0ddecbd221bdb5d7e5c4fcfce620412a26cccc20a0c4250c59d44`) shows the document chooser. They
do not show a usable direct control for `ui.fontScale`. The setting is present in `system://ui` and
is read at startup, but the previous native affordance check did not reach an enlarged-text
configuration through the interface. `F2` is a pouch toggle; focused ZigZag cell text editing is
bound to `E`. Treat a possible `F2` parameter editor as unverified until a real native control or
bound action is demonstrated. The upcoming enlarged-text run needs to prove the actual user path and
inspect the resulting frame; a launch flag or configuration edit alone is not a user affordance.

## Recommendation for the next runnable scope

Use an actual UI-authored selection that crosses at least two pages. Enter the exact Link endpoint
and record its selected document, Link/member identity, byte range, caret/anchor, current visit and
source/activity snapshots. Confirm that the initial view prioritizes the exact beginning, with a
full readable line clear of chrome. Then use an ordinary visible or default input path to pan/scroll
until the later selected page is readable. The selection and Link identity should remain exact
throughout, no automatic refit should return the view to page one, and navigation should not add a
visit. Return/Back behavior remains governed by the existing command boundary. Compare source dumps
before and after; distinguish expected private activity checkpoint writes from completed visits.

The scope should demonstrate partial and completed movement, a tighter viewport, and one genuinely
enlarged text run at the native setting’s interface-driven value. A range too large for one frame
must remain a readable scrolling selection, not be reduced below the configured text floor. Keep
every fit/retry guarded by document identity, view generation, exact range and current Link state so
stale queued work cannot pull the reader back after the user moves the camera. If required pages are
not built yet, defer until the existing document readiness seam can establish geometry; do not
invent placeholder bounds or block the UI thread shaping pages.

No source operations or reader visits should be emitted by panning, scrolling, zooming or text
reflow. Reflow can change page assignment and screen geometry, so the exact byte range must remain
the invariant while page anchors are reacquired from the new layout. Keep visual decoration behind
readable glyphs, retain the Link dock hierarchy and hue, and review actual backend captures for
selection contrast and edge clipping. A screenshot at entry alone cannot establish that a user can
continue through the multipage selection.

## Review gates

- Inspect native captures and logs from the interface-driven font-size path and from multipage
  entry, ordinary pan/scroll, zoom and return. Record PNG hashes and the run/profile provenance.
- Compare exact selection/caret and Link/member identity before and after camera movement and
  reflow. Confirm source operation snapshots remain unchanged and completed-visit counts change only
  on explicit Enter.
- Review narrow and enlarged typography for legible text, chrome overlap and honest continuation;
  include the backends actually available to the run. The fourth-increment 800×600 backend parity
  does not imply parity for new layouts.
- Keep cancellation/unavailable/reopen J24/J25 variants, native AT-SPI, optimized performance
  acceptance, and the complete 16+16 journeys open. No whole-journey or accessibility claim follows
  from a native frame or diagnostic tree alone.

I will add the independent final UX/aesthetics judgment after the fifth-increment native captures
and their hashes/provenance are available.

## Fifth-increment prerequisite and occurrence review

The bounded native occurrence replay is supported by `evidence/occurrence/validation.md` and its
native frames. The fixture UI-authored Link 21 with two left members and one right member. The
tablet retained the current reading place and visit 1 while the reviewer selected a member and
crossed sides. The two source operation dumps are byte-identical before and after. The activity dump
is also identical before and after preview/cancel; explicit Enter adds visit 2, and Activity Back
restores visit 1. This is useful evidence for member selection and preview semantics, not a complete
J24 journey.

Two variants remain unexercised. Every authored member in this fixture reports `place 1/1`, so the
`Alt+Shift+L` step cannot demonstrate repeated-occurrence browsing. `Alt+Shift+Escape` ran at the
origin with no pending endpoint, so it does not demonstrate cancellation during unavailable or
pending entry. The later `Ctrl+W` closed the empty/default tab without prompting, so its following
Escape does not establish a close-cancel path. Keep these explicitly open.

The captures are crisp and consistent with the existing palette: the active side uses the same cyan
cue as the selected Link context, muted controls stay secondary, and the text selection remains
visible above the expanded tablet. At 1024×768, the tablet occupies much of the canvas and overlays
the blank part of the page while preserving the selected sentence. Its orderly control grid and
readable labels suit this information-dense state. This review does not treat its solid slate
surface as a new visual rule or infer how it behaves at enlarged typography. All six inspected PNGs
under `evidence/occurrence/selected/` match their archived PPM frames pixel-for-pixel, with zero
differing pixels; the PPM hashes are recorded alongside the frames.

## Pre-build review: UI font scale form

The proposed action is appropriately narrow: one `UI font scale` form updates the existing typed
`double` in `system://ui`, through `SystemStoreModel::updateSetting`, and then
`Session::repointSystemDoc()` saves it and invokes the existing live UI configuration callback. It
does not parse generic numeric cells or edit whichever store happens to be open. The command name,
default chord `Ctrl+Alt+Shift+F`, action label and `system://ui` Notes explanation provide a
keymap-driven route. The note tells readers this changes chrome typography and leaves document text
size alone. This is a reasonable prerequisite for an actual enlarged-chrome run.

The current code in progress guards that path carefully: it requires one double setting, seeds the
field using 17 significant digits, parses the entire answer, rejects non-finite and non-positive
values, and rejects values that cannot survive conversion to the UI's `float` type before minting
setting operations. Its typed update and system-document repoint avoid visited-document mutations
and reader visits. These are the right boundaries to retain. A valid update should visibly affect
live chrome before closing and persist through a fresh launch with the same profile.

One small usability issue remains in the current implementation: `Form` closes before calling its
accept callback. Invalid text then produces an error notification, but dismisses the field value and
requires reopening the action to correct it. This preserves the setting but costs an avoidable
re-entry; an inline form complaint or a reopened form retaining the attempted value would be
clearer. If unchanged, confirm the error notification is clear and that reopening the form changes
no setting or source data.

The Notes text change lives in `defaultSystemDocNotes()`, so an already-existing profile will keep
its prior Notes prose unless the implementation explicitly updates it. The new default keymap entry
is added through the setting-spec path and should remain visible in the keymap document. Confirm
that users with an existing profile can still discover the chord there; do not claim their Notes
page was updated just because a fresh profile receives the new text.

This is a source-level review only. The form has not yet been judged from a native frame. Review the
accepted enlarged-scale frame for clipped labels, lost controls or an unreadable selected passage,
and verify invalid input, Escape, persistence and source-store isolation through the actual UI. I
will record that visual result separately after the fresh captures arrive.
