---
name: zigzag-ui-design
description: >-
  Expert workflow and runbook for comprehensive UI design in the zigzag multidimensional Xanadu visualizer.
  Use when designing or implementing multi-view layouts, Cell Content View vs Topology View projection modes, dimension cycling, rank transitions, and preflet integration.
---

# `zigzag` Multidimensional UI Design Workflow

This skill defines the technical procedures, projection mathematics, view-switching modes, and interaction models for the `zigzag` multidimensional Xanadu visualizer application.

## 1. Core Architectural Concept

Zigzag visualizes hyperdimensional zzstructures (where each cell can have $+1$ and $-1$ links across an arbitrary number of dimensions $d.1, d.2, d.3 \dots$). To resolve the intrinsic tension between detail and structural topology, Zigzag employs a **Multi-View Architecture**:

---

## 2. The Two Primary View Modes

### Mode A: Cell Content View (Detail & Active Alignment)
- **Primary Goal**: Reading, inspecting, and editing cell content at full richness.
- **Dynamic XYZ Alignment**:
  - The currently active cell sits at the local focus origin $(0, 0, 0)$.
  - Neighbor cells along the active $X$-dimension ($d.x$), $Y$-dimension ($d.y$), and $Z$-dimension ($d.z$) smoothly translate to form tight, legible orthogonal cross-hairs intersecting directly at the active cell.
  - Cells dynamically expand their width and height to fit full text and embedded media without truncation.
  - Non-immediate cells along the rank are positioned with soft spring physics, even if this distorts the global lattice topology.
- **Visual Style**: Rich card styling, elevated focus border, full HarfBuzz-shaped typography, preflet action buttons (`[Fetch Magnet]`, `[Inspect Preflet]`).

### Mode B: Topology View (Macro-Structure & Lattice Regularity)
- **Primary Goal**: Global structural comprehension, finding dimensional cycles, and visualizing complex graph manifolds.
- **Fixed-Size Cell Glyphs**:
  - Every cell is rendered as a uniform, fixed-size isometric tile / rounded pill (e.g. $120 \times 60\,\text{px}$).
  - Cell text is abbreviated (first 16 characters or iconic category glyph: chapter, note, clone, preflet).
  - Strict geometric lattice grid spacing ($S_x, S_y, S_z$) preserves true hyperdimensional connectivity without distortion.
- **Visual Style**: Clean isometric projection, glowing dimension ribbons, dimension ring indicators, clone indicator badges ($[⇄]$), and global structural symmetry.

---

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
- When moving cursor via Arrow Keys (`Left/Right` for $d.x$, `Up/Down` for $d.y$, `PgUp/PgDn` for $d.z$):
  - Uses `Choreograph` spring damping:
    $$\mathbf{P}_{\text{cell}}(t) = \mathbf{P}_{\text{target}} + e^{-\zeta \omega_n t} (\mathbf{A} \cos(\omega_d t) + \mathbf{B} \sin(\omega_d t))$$
  - Focus scale pulse: Active cell scales up by $1.25\times$ upon arrival.

---

## 4. Implementation Checklist for New Features

1. **View Mode State**: Add `enum class ViewMode { CellContent, Topology, Matrix }` in `apps/zigzag/zigzag_visualizer.hpp`.
2. **Dynamic Geometry Generator**: Implement cell size calculation based on `viewMode` in `zigzag_visualizer.cpp`.
3. **Lattice & Dimension Shaders**: Ensure fixed-size tiles use instanced quad batching with dimension color coding.
4. **Picking & Accessibility**: Expose both Content and Topology cell nodes to `a11y::Builder` with current coordinate values.
