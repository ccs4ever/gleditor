# Zigzag Multidimensional UI Design Workflow & Multi-View Specification

## 1. Executive Summary

This specification establishes the comprehensive UI/UX architecture and agentic workflow for `apps/zigzag` — the multidimensional visualizer and navigator for Project Xanadu zzstructures.

Zigzag resolves the fundamental tension between **Cell Content Density** and **Topological Manifold Comprehension** through a dual-mode visual projection engine.

```
       [ MODE 1: Cell Content View ]                     [ MODE 2: Topology View ]
       ----------------------------                     ------------------------
       • Expanded rich text & media                     • Uniform fixed-size isometric tiles
       • Dynamic auto-sizing                            • True hyperdimensional lattice grid
       • Soft XYZ spring alignment to focus             • Structural clarity & dimension rings
       • Optimized for Reading & Editing               • Optimized for Graph Navigation
```

---

## 2. Multi-View Architecture

### 2.1 View Mode 1: Cell Content View (Detail & Active Alignment)
- **Design Objective**: Provide an uncompromised reading, editing, and multimedia inspection experience for individual cells.
- **Dynamic Content Sizing**:
  - Cell quads dynamically expand their width ($W_{\text{cell}} \in [200, 600]\,\text{px}$) and height to accommodate multi-line HarfBuzz-shaped text and media embeds.
  - Padding, line height, and font metrics adapt to cell content volume.
- **Dynamic XYZ Spring Alignment**:
  - The focused cell is locked to the local origin $(0, 0, 0)$.
  - Immediate neighbor cells along the bound $X$-axis ($d.x$), $Y$-axis ($d.y$), and $Z$-axis ($d.z$) are pulled by strong spring forces into collinear cross-hair alignment with the focused cell.
  - This alignment guarantees that the immediate dimensional neighborhood of the focus can be read in a clean, linear sequence, even if this locally distorts or overlaps distant branches of the topology.

### 2.2 View Mode 2: Topology View (Macro-Structure & Lattice Regularity)
- **Design Objective**: Provide a clean, unoccluded view of the global zzstructure topology, dimensional cycles, and rank intersections.
- **Fixed-Size Isometric Tiles**:
  - Every cell is rendered as a uniform geometric tile (e.g. $140 \times 65\,\text{px}$) regardless of content length.
  - Cell text is truncated to a compact 18-character summary with category icon badges (`[chapter]`, `[note]`, `[preflet]`, `[clone]`).
- **Rigid Lattice Coordinate Grid**:
  - World position $\mathbf{P}(c)$ is strictly determined by dimensional rank indices $(r_x, r_y, r_z)$:
    $$\mathbf{P}(c) = r_x \cdot \mathbf{S}_x + r_y \cdot \mathbf{S}_y + r_z \cdot \mathbf{S}_z$$
  - Preserves geometric symmetry, parallel ranks, and hypercube topologies.

---

## 3. Interaction Mechanics & Dimension Binding

### 3.1 Mode Switcher HUD
- Located at the top-center of the screen:
  - `[ 📄 Cell Content View ]` (`Key: 1` or `Key: V`)
  - `[ 🌐 Topology View ]` (`Key: 2` or `Key: T`)
  - `[ 📊 Projection Matrix ]` (`Key: 3` or `Key: M`)

### 3.2 Dynamic Dimension Binding Carousel
- HUD indicators at the bottom-left show the active 3D axis bindings:
  - **X Axis**: Dimension $d.1$ (e.g., `"d.sequence"`) — *Cyan*
  - **Y Axis**: Dimension $d.2$ (e.g., `"d.category"`) — *Emerald*
  - **Z Axis**: Dimension $d.3$ (e.g., `"d.version"`) — *Amber*
- Controls:
  - `Tab`: Cycle X/Y dimensions.
  - `Shift+Tab`: Cycle Y/Z dimensions.
  - `Ctrl+D`: Open interactive dimension picker modal.

### 3.3 Preflet & BitTorrent Resolution
- Cells with attached Preflets display a magnet badge (`[🧲]`).
- Activating a preflet initiates asynchronous BitTorrent fetching via `PrefletFetcher`.
- When resolved, the target Slice smoothly materializes as an adjacent connected 3D cluster.
