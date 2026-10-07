# UI text fitting: batch 9

The final batch removes the modal compatibility dispatchers and the mutable Canvas text-width limit.
Form, radial menu, the application overlays and ZigZag input now implement FocusScope directly. Xuzz
holds registration handles for their lifetimes, with an explicit quit command policy. The manager
continues to own activation order, traversal, modal isolation and pointer capture. Render-thread GPU
picks go to that same focused scope without a downcast.

Composed text enters scopes as string views. The existing key convenience overloads remain for
callers, while manager dispatch uses KeyEvent. A caller must release its registration before tearing
down a scope. The former per-event registration synchronization and composite dispatcher are gone.

Canvas measurement and coordinate-based text drawing remain unbounded single-line operations.
Callers needing a width budget use boxed addText with TextFit, including the migrated regression
case. Overlay constructors use empty overrides and typed font roles; geometry canvases obtain a role
description from the shared theme. Collaborator labels follow the live caption role.

The new tools/check-ui-text-policy.py gate runs from make lint. It tokenizes C++ source, ignores
comments and checks multiline expressions. Production byte-prefix substrings followed by an ellipsis
are rejected. UI font descriptions with a literal Sans point size are rejected in overlay, shared UI
and named presentation components. Font families in typed themes and parser substrings are allowed.
Four scanner regression tests cover positive, negative, multiline and command-line failure cases.
Accessibility tree dumps now retain full values instead of cutting UTF-8 at a byte count.

The native system://ui schema and notes templates describe scope ownership, bounded labels, full
accessible names and pouch resizing. InitializeSystemStore regenerates the two pages with native
heading format links, their forced page break and the bidirectional comment link. Its regression
checks the new text as well as that structure; no Markdown or on-disk format change is introduced.

Validation uses the complete headless test suite, format-check, lint and compare-backends. A direct
Form regression checks stacked modal restoration, composed Unicode text, command restrictions and
unregistration. Existing named-control application journeys exercise every modal and restoration of
document editing. Backend captures are inspected on a private virtual display; GLES matches OpenGL
exactly and Vulkan differs on 0.165% of pixels, within the 1% threshold. Atlas growth, overlay,
culling, picking and small-zoom comparisons pass.

All gates passed. The suite passed 740 library, 1,257 xanadoc, 61 Xuzz, 184 ZigZag, 12 rootless
swarm and one publication DHT test. The existing video-card test remains skipped. The previously
recorded intermittent publication seed-review failure did not recur.
