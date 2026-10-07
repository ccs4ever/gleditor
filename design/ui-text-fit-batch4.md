# UI lift: batch 4 — core widget migrations

Batch 3b is committed as `d2e24bd`. This batch moves the six core consumers onto shared metrics,
theme typography, responsive layout, explicit boxed text fitting and retained geometry. Specialized
radial gestures and document/media transforms remain in their components.

Form derives its panel width, spacing and row heights from font metrics and clamps to the safe area.
Narrow layouts stack field labels above controls. When all rows cannot fit, traversing to a field
reveals its viewport while retaining every answer. Small windows give the focused input priority
over supporting headings; expanded choices reserve visible space for the highlighted option. Open
choice lists use the shared virtual List and follow the keyboard highlight. Text fields use the
shared TextField fitting, horizontal scroll and grapheme editing. Accessibility and input-method
rectangles come from the same retained boxes; secrets remain masked on the accessibility bus even
when revealed on screen. Steady frames submit retained buffers without copying the fields or
reshaping text.

Toast fits wrapped messages into a retained, safe-area stack and keeps each message's expiration and
fade behavior. DocSwitcher lays out tab titles alongside reserved close and utility controls;
accessibility retains full titles when the visible text is shortened. Its captured picks bind to
stable document identities, so a later reorder or removal cannot redirect a click.

RadialMenu preserves its wedge gestures, submenu behavior and action identities while fitting labels
and descriptions into contained boxes. FloatingToolbar3D uses responsive button flow within the safe
area and preserves its existing screen projection. Both guard event/render state and invoke user
callbacks outside their locks. Toolbar picks retain the document context captured with the frame;
changing or closing that document rejects stale actions.

MediaWidget keeps its caller-provided page/world transforms and dimensions. Its fitted title, time,
buttons and scrubber share retained layout and accessibility boxes. Static chrome is separate from
live video/progress drawing; playback state, mute, rate, elapsed seconds and presentation changes
invalidate only the required geometry. Compact GPU indices map to full accessibility identities.

Empty constructor font overrides now use the live theme role. Explicit legacy font descriptions keep
their family, style and point size, with content, UI and font scale applied by the shared
`scaledFontDescription` helper. The plain editor's chrome uses its UI theme independently of the
document font. Its existing TSV configuration gains `ui.scale`, `ui.fontScale`,
`ui.safeMarginShare`, `ui.minTouchPx`, `ui.minFontPx` and per-role font fields
`ui.font.<caption|label|body|title|mono>.<family|points>`. Scale and font values must be positive
and finite; safe margin share is between 0 and 0.5. The existing config format is preserved in this
batch; replacing its TSV loader with YAML is a separate migration.

Retained overlay pick scopes occupy a separate range from frame-local scene scopes. Captured picks
preserve their target meaning when live geometry or document order changes. A missing or removed
target cannot activate another control.

Retained font caches exposed a shutdown lifetime defect: a widget could retain a render-thread font
after that thread's FontManager destroyed its FreeType library. FontManager fonts now share library
ownership, and a regression shapes and releases a font after its creating thread exits. Raw-library
FontFace callers retain their existing caller-managed lifetime contract.

Backend validation also exposed Vulkan completion semaphores being reused after submission finished
but before presentation consumed them. Presentation completion semaphores now belong to swapchain
images; reacquiring an image establishes safe reuse, and swapchain teardown releases them after
waiting for the device.

## Verification

Focused headless widget, Form, configuration and pick-scope regressions passed. Coverage includes
tiny windows and independent font/display scales, full accessibility labels, masked secrets,
retained geometry, long choices, grapheme editing, owner-qualified actions, callback reentrancy,
removed documents and stale picks after tab switches and reorderings.

A no-op render-device probe measured 1,000 warm frames per consumer. All six consumers performed
zero C++ allocations, buffer/texture uploads, pipeline creation, layout or HarfBuzz calls. CPU
submission p95 ranged from 100 ns for Form to 321 ns for a stopped MediaWidget. These numbers
exclude GPU work, malloc-only third-party allocations, background work, active playback and document
attachment costs. The backend benchmark reports separate serialized CPU/GPU wall times and labels
core scenarios as `theme-default`, since their typography comes from the live theme rather than its
standalone `--font` argument.

The complete build and `make -j$(nproc) test` passed: 731 library tests, 1,252 engine tests, 57 Xuzz
tests, 119 ZigZag tests, 12 swarm tests and the publication-network test. The engine's existing
video-card screenshot skip and disabled test remain unchanged. SDL2 application integration
compiles. Repository-wide formatting and lint pass.

Strict headless OpenGL, OpenGL ES and Vulkan comparison passes. Captured frames were inspected for
visible labels, flat baselines and containment. OpenGL and OpenGL ES core-widget captures are
pixel-identical. Vulkan differences exceeding one channel level affect at most 0.14% of pixels,
within the existing 1% tolerance; Toast and the retained overlay match exactly.

For 100 measured frames after 10 warmup frames, the following are serialized software-rendering p95
wall times in milliseconds (including `waitIdle`). All six core scenarios report zero steady-state
layout, HarfBuzz and fallback shaping calls. Overflow columns for mixed core scenes are
intentionally unmeasured; their geometry regressions cover containment instead.

| Consumer              | OpenGL | OpenGL ES | Vulkan |
| --------------------- | -----: | --------: | -----: |
| Toast                 |  4.213 |     3.777 |  7.966 |
| Form                  |  6.822 |     6.818 | 12.293 |
| DocSwitcher           |  4.036 |     4.018 |  7.840 |
| FloatingToolbar3D     |  4.102 |     4.311 |  8.645 |
| RadialMenu            |  5.070 |     5.051 | 10.794 |
| MediaWidget (stopped) |  4.503 |     4.362 |  8.528 |

These new core rows establish measurements for later batches; stage 0 did not capture individual
consumer timings. They do not establish a direct before/after p95 comparison for those consumers.
