# Independent drawer UX and aesthetics review: initial constraints

Reviewed 2026-10-14 against first-increment commit `4f2d3fac`. This reviewer applies the Xuzz UI
design and UX validation skills, read both previous final reviews, and edits no product code. This
initial pass identifies constraints; visual acceptance requires completed native captures after the
fix.

## Observed interface failure

In `/workspace/spatial-build/backend-final/xudu_opengl.log`, opening the Store Object Manager
produces named accessibility entries for Close Drawer, New Slice and New Xanadoc. The next native
`--click-label "New Xanadoc"` fails because there is no visible actionable control with that name. A
semantic entry alone does not make this operation reachable. The isolated fitted-row check also
finds empty glyphs and zero visible bytes across window sizes, scales and font families. These are
concrete blockers to creating documents through the drawer, not proof of a complete journey
regression.

The retained implementation builds its header, creation row and object lists through shared widgets,
theme metrics and ScreenOverlay. Existing action queues hold structure birth identities and drawer
generations. The bounded repair should retain those mechanisms rather than introducing another
drawer, command path or stored structure.

The lead's control-specific diagnostic identifies a Close Drawer rectangle with zero height: its
header wraps to a second row without space for that row. The existing widgets use Label as their
default font role, so a font-role mismatch is not an established cause here. The header's measured
fractional width and subsequent pixel rounding are the active repair target. The unchanged pinned
baseline also reproduces the missing New Xanadoc behavior; this is a pre-existing interface blocker.

## Required design behavior

- Close Drawer, New Slice and New Xanadoc remain visible, named and independently actionable at
  supported sizes and scales. Pointer, keyboard focus and native accessibility actions must resolve
  the same retained identities.
- Fit labels inside actual content rectangles after nested container padding, per-widget font roles,
  gaps and pixel rounding. A one-row allocation must not let an unexpected wrap leave a control with
  zero height.
- Preserve full accessible names when visible object labels shorten. Object lists scroll within the
  available area; creation and dismissal remain reachable.
- Keep store creation semantics and stable birth/generation checks unchanged. Reject stale actions
  after closing and reopening; expose cancellation without writing.
- Keep the current theme-derived dark surface, rectangular geometry, type hierarchy and shared
  content insets. Resolve layout from measured metrics rather than a screenshot-specific width or
  offset.

## Required actual evidence

Capture the populated drawer at a small window, normal window and enlarged text, plus a dense
scrolled state. Read each image for complete creation/dismissal labels, legible rows, aligned
columns and safe bounds. Exercise New Xanadoc and New Slice through real named controls, confirm
resulting births and source text preservation, then close and reopen. Record keyboard
focus/dismissal and full native accessibility names separately from an AT session, which this build
does not support.

Compare completed captures across OpenGL, OpenGLES and Vulkan for the changed drawer. Baseline
editor backend images cannot establish drawer parity. Passing layout checks alone cannot establish
aesthetic acceptance. This review does not accept J1–J16 preservation or any complete J17–J32
journey.
