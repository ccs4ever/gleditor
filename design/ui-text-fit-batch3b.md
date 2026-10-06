# UI lift: batch 3b — layout-driven focus and accessibility

Focus scopes can now publish an immutable `ui::LayoutResult`. Its focus order and box metadata
identify text fields, control groups and default actions. `FocusManager` selects the initial node,
wraps Tab and Shift+Tab, moves arrows within a group after offering editing to the control, and
routes Return and Escape. A coherent focus snapshot supplies the selected scope, node, modal state
and revision to accessibility. Each scope retains its node while a newer modal covers it.

Pointer picking and control accessibility requests use a one-shot focus request without storing a
manager pointer. Requests are validated against the current layout. Input-method placement is
enabled only for a focused text field. Legacy scopes without layout snapshots retain their existing
input behavior until the later consumer migrations.

Form publishes the same field rectangles it draws and uses for accessibility. Its private `step()`
implementation is removed. Direct legacy key calls use the shared traversal helper; managed key
calls let the manager traverse. Dropdown option selection remains Form's responsibility, including
settling a highlighted option when Tab leaves a field.

ScreenOverlay is a focus scope. Focus rings and text carets use the retained boxes, and pointer
press/release, list scrolling and scrubber dragging follow the same identities. Focus changes update
geometry without reshaping text; unchanged frames retain both layout and drawing data.

Canvas pick tags have a 16-bit index, while widget identities are 32-bit. Each retained scene maps
compact pick indices to full widget identities. An asynchronous pick captures that immutable mapping
with its scene, so reordering controls before the result arrives cannot redirect the action. A
removed target consumes the old result without activating another control.

The accessibility publisher follows the focus manager bound by AppState. It clears source-provided
modal flags, marks one active modal root, and places focus within that root. Multiple roots are
grouped under a manager-owned dialog; scopes without accessibility sources receive a dialog
placeholder. Legacy sources can nominate their own child focus, which is accepted only within the
active scope's subtree. Background nodes lose focusability and actions while a modal is active.
Action dispatch also checks live modal ownership before invoking a source, so opening a modal blocks
background requests before the next tree rebuild.

Publisher callbacks run outside its registration and tree locks. Raw sources remain caller-owned and
must stay alive until in-flight callbacks finish. Dynamically replaced media widgets register with
shared ownership, retained by registration snapshots and action callbacks. Tree invalidation
includes registration changes, window dimensions and manager focus revision, as well as source
revisions. An injectable platform adapter permits real queued-action tests without a desktop
accessibility service.

The inherited focus fields exposed stale transitive include lists in incremental builds. Dependency
files now name themselves as targets and depend on the Makefile, so header changes regenerate their
include lists before objects are rebuilt. This prevents callers retaining an old class layout after
a header adds a new dependency.

## Verification

Tests cover initial focus policies, forward and reverse traversal, grouped arrows, default actions,
nested node restoration, editing precedence and invalid focus requests. Overlay tests cross window
sizes and font scales and compare traversal with the actual layout order: 640 × 360, 1280 × 720 and
2560 × 1440, with font scales 0.8, 1, 1.5 and 2 and content scales 1, 1.25 and 2. Deterministic
thread-latch tests exercise stale layout selection, reopened scopes and delayed focus callbacks. A
retained-draw test changes focus 50 times without rebuilding glyph pipelines or reshaping text.
Delayed-pick tests cover full identities, reorderings and removed controls. Accessibility tests
cover one-modal ownership, namespaced child focus, source-less and multiple-root scopes, background
action blocking and reentrant source callbacks. The existing binary modal-isolation table also
checks that each open-modal accessibility snapshot reports exactly one modal root.

The allocation probe performs 1,000 warm overlay preparations with zero allocations and an unchanged
layout revision. An unbound accessibility publisher also performs 1,000 cached rebuilds without
allocations. A focus-manager snapshot currently allocates once per read; a bound publisher inherits
that allocation, while its source descriptions remain cached. This is a remaining cost, not a claim
that the complete focus and accessibility path is allocation-free.

Final validation: the complete build and `make -j$(nproc) test` passed: 684 library tests, 1,252
engine tests, 57 Xuzz tests, 119 ZigZag tests, 12 swarm tests and the publication-network test. The
isolated SDL2 application compile passed. Repository lint and formatting of changed C++ files
passed. The subsequent separate formatting cleanup (`2b92584`) corrected 45 untouched C++ files and
three shaders; repository-wide `format-check` now passes.

Headless OpenGL, OpenGL ES and Vulkan comparison passed. The retained overlay captures match
pixel-for-pixel, with no detected glyph overflow. For 36 labels over 100 measured frames after 10
warmup frames, p50/p95 frame times were 5.878/6.296 ms on OpenGL, 5.844/6.308 ms on OpenGL ES and
12.336/13.316 ms on Vulkan. Steady-state layout, HarfBuzz and fallback shaping counts were zero.
These are software-rendering measurements in the private Xvfb environment.
