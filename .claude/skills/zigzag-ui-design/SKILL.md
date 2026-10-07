---
name: zigzag-ui-design
description: >-
  Expert workflow and runbook for UI design of ZigZag slice views in xuzz (zigzag is no longer a separate application).
  Use when designing or implementing multi-view layouts, Cell Content View vs Topology View projection modes, dimension cycling, rank transitions, and preflet integration.
---

# ZigZag Slice View UI Design Workflow

This skill defines the technical procedures, projection mathematics, view-switching modes, and
interaction models for ZigZag slice views in `xuzz`. `zigzag` is retired as a separate application:
`build/zigzag` is a symlink to `xuzz`, and `apps/zigzag/` holds components that `xuzz` links.

New slice views are specified in [`design/view-system.md`](../../../design/view-system.md): a view
is a registered `View` over a `ViewManifold`, with pure layout in `apps/common/xanadu/view/` and
renderer wiring in `apps/xuzz/`. Sections 2 and 3 below describe the two presentations
`ZigzagVisualizer` implements today; the Matrix View in §3 was never built.

## 1. Core Architectural Concept

A slice view visualizes hyperdimensional zzstructures (where each cell can have $+1$ and $-1$ links
across an arbitrary number of dimensions $d.1, d.2, d.3 \dots$). To resolve the intrinsic tension
between detail and structural topology, `xuzz` employs a **Multi-View Architecture**:

______________________________________________________________________

## 2. The Two Primary View Modes

### Mode A: Cell Content View (Detail & Active Alignment)

- **Primary Goal**: Reading, inspecting, and editing cell content at full richness.
- **Dynamic XYZ Alignment**:
  - The currently active cell sits at the local focus origin $(0, 0, 0)$.
  - Neighbor cells along the active $X$-dimension ($d.x$), $Y$-dimension ($d.y$), and $Z$-dimension
    ($d.z$) smoothly translate to form tight, legible orthogonal cross-hairs intersecting directly
    at the active cell.
  - Cells dynamically expand their width and height to fit full text and embedded media without
    truncation.
  - Non-immediate cells along the rank are positioned with soft spring physics, even if this
    distorts the global lattice topology.
- **Visual Style**: Rich card styling, elevated focus border, full HarfBuzz-shaped typography,
  preflet action buttons (`[Fetch Magnet]`, `[Inspect Preflet]`).

### Mode B: Topology View (Macro-Structure & Lattice Regularity)

- **Primary Goal**: Global structural comprehension, finding dimensional cycles, and visualizing
  complex graph manifolds.
- **Fixed-Size Cell Glyphs**:
  - Every cell is rendered as a uniform, fixed-size isometric tile / rounded pill (e.g.
    $120 \times 60\,\text{px}$).
  - Cell text is abbreviated (first 16 characters or iconic category glyph: chapter, note, clone,
    preflet).
  - Strict geometric lattice grid spacing ($S_x, S_y, S_z$) preserves true hyperdimensional
    connectivity without distortion.
- **Visual Style**: Clean isometric projection, glowing dimension ribbons, dimension ring
  indicators, clone indicator badges ($[⇄]$), and global structural symmetry.

______________________________________________________________________

## 3. View Switcher & Interaction Design

### Mode Switcher HUD

- A floating mode toggle bar located at the top-center:
  - `[ 📄 Cell Content View ]` (Hotkey: `V` or `1`)
  - `[ 🌐 Topology View ]` (Hotkey: `T` or `2`)
  - `[ ⚙ Matrix View ]` (Hotkey: `M` or `3`)

### Dimension Axis Carousel

- HUD indicators at the bottom-left showing current axis mappings:
  - **X-Axis**: `d.1 (Linear Sequence)` [Color: Cyan]
  - **Y-Axis**: `d.2 (Category / Type)` [Color: Emerald]
  - **Z-Axis**: `d.3 (Time / Version)` [Color: Amber]
- Cycling controls (`Tab` to cycle X/Y, `Shift+Tab` to cycle Y/Z, `Ctrl+D` for dimension selector).

### Rank Transition Choreography

- When moving cursor via Arrow Keys (`Left/Right` for $d.x$, `Up/Down` for $d.y$, `PgUp/PgDn` for
  $d.z$):
  - Uses `Choreograph` spring damping:

    ```math
    \mathbf{P}_{\text{cell}}(t) = \mathbf{P}_{\text{target}} + e^{-\zeta \omega_n t} (\mathbf{A} \cos(\omega_d t) + \mathbf{B} \sin(\omega_d t))
    ```

  - Focus scale pulse: Active cell scales up by $1.25\times$ upon arrival.

______________________________________________________________________

## 4. Implementation Checklist for New Features

1. **Register a view, do not add an enum case**: a new presentation is a `ViewDescriptor` in
   `ViewRegistry` (`design/view-system.md` §8.1), not another `ZigzagVisualizer::ViewMode` value.
   Add no files under `apps/zigzag/`.
1. **Pure layout in the engine**: implement `layout(const LayoutInput &, LayoutSink &)` under
   `apps/common/xanadu/view/`, with content sizes from the input's measurer, and test it from
   `xuzz_test` with no graphics device.
1. **One link choke point**: any cell the view mints goes through `ViewManifold`, which keeps at
   most one neighbour per direction per dimension and tosses view cells on rebind.
1. **Lattice & Dimension Shaders**: Ensure fixed-size tiles use instanced quad batching with
   dimension color coding; draw calls live in `apps/xuzz/`.
1. **Keymap, Picking & Accessibility**: every action ships with a default chord in
   `system://keymap`; expose cell nodes to `a11y::Builder` with current coordinate values, and give
   view-only cells a role other than `cell`.
