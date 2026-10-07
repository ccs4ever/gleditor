# UI text fitting: review follow-up

Three restarted reviewers found duplicate pointer activation, stale modal picks, missing native Xuzz
typography controls, lint bypasses, unbounded diagnostic text and collaborator badge overflow. This
follow-up fixes those findings in the isolated UI worktree.

Pointer dispatch returns either CPU consumption or an explicit GPU request. Retained screen controls
activate on release; their consumed presses request no GPU activation. Radial and ZigZag world
scopes explicitly opt into GPU picking. Mouse, touch and scripted clicks follow the same route. GPU
clicks retain a registration identity, opening sequence and focus revision, both while queued and in
the request record. Submission and completion discard obsolete origins; completions match request
IDs instead of coordinates. Document additions and closures also invalidate pending origins. Pointer
position and origin are transferred together under one mutex.

Regression checks cover one activation per press/release, release away from a button, outside modal
blocking, delayed answers after closure/reopening/scope changes/unregistration/focus loss, pending
click replacement and changed document membership. The existing application named-control and
modal-isolation journeys exercise the complete event and renderer path.

Native system://ui now seeds and loads ui.scale, ui.fontScale, ui.safeMarginShare, ui.minFontPx,
ui.minTouchPx and five ui.font.role.family/points pairs. Defaults derive from the shared typed
Theme; invalid runtime lengths fall back. Xuzz applies immutable theme snapshots and scale settings
at launch and when the system document changes. Named link-panel font roles follow the live theme;
empty and explicit legacy descriptions remain accepted. Existing UI stores add missing setting cells
with native schema/default/notes metadata, preserve text pages, user notes and existing values, and
mint no operations on subsequent unchanged loads. Fresh stores include updated companion pages with
native heading format links and their bidirectional comment link.

The lint tokenizer treats character literals as opaque, decodes ordinary and raw strings, combines
adjacent literals, recognizes zero integer suffixes and covers the floating toolbar and other UI
presentation files. Seven tests exercise positive and negative cases and command-line failure.
Accessibility diagnostic values have a bounded complete-grapheme preview; actual accessible values
and labels remain complete. Shared private UTF-8 and Indic conjunct helpers retain text fitting's
existing behavior. Diagnostic tests check Unicode boundaries, oversized graphemes and a ten-MiB
value without shaping or copying that entire value into the dump.

Collaborator nameplates fit text from live caption glyph metrics, grow their bounds for typography,
and clamp to the shared safe area. Full semantic names are retained separately from visual fitting.
Anchor-only movement reuses shaping, and expired collaborators release cached presentation state.
Tests sweep 144 font/window/scale combinations, 100 warm anchor moves, nonfinite anchors and a tiny
viewport.

An existing UI store whose native structure cannot be resolved against the active permascroll is
left unchanged with a settings warning. The upgrade does not mint replacement dimensions or write to
a mismatched scroll. A regression checks both the original stored text and operation count. The
pouch divider journey now focuses the handle and uses its Right key to resize: a stationary
press/release starts and ends a drag without changing the width.
