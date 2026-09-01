# gleditor 3D UI Design Workflow & Technical Specification

## 1. Executive Summary

This specification establishes the comprehensive UI/UX architecture and agentic workflow for `apps/gleditor` — elevating the traditional text and document editor into an immersive 3D spatial environment.

```
                  +---------------------------------------------------+
                  |  [+] [📂] [💾] [✕] | [B] [I] [U] [S] | [H1][H2]   | <-- Floating 3D HUD Toolbar
                  +---------------------------------------------------+
                                            | (Elevation: +12px Z, +28px Y)
                                            v
                                 +---------------------+
                                 |  Active Document    |
   [Doc #0 Orbit: -30 deg]       |  (Z = 0, Scale 1.0) |       [Doc #2 Orbit: +30 deg]
   (Z = -40px, Opacity 0.42)     |                     |       (Z = -40px, Opacity 0.42)
                                 +---------------------+
```

---

## 2. 3D Spatial Document Management & Tab Navigation

### 2.1 Spatial Slot Arrangement
Unlike traditional 2D tab bars that clip and hide document state, each document in `gleditor` exists within its own spatial coordinate slot along a subtle cylindrical orbit in 3D world space:

- **Active Document ($i = A$)**:
  - Center position: $(0, 0, 0)$.
  - Rotation: $(0, 0, 0)$.
  - Opacity: $1.0$.
  - Interactivity: Full caret placement, text selection, and HarfBuzz shaped editing.

- **Background / Peripheral Documents ($i \neq A$)**:
  - Relative angular offset: $\Delta \theta_i = (i - A) \cdot \theta_{\text{slot}}$, where $\theta_{\text{slot}} = 26^\circ$.
  - Spatial coordinates:
    $$X_i = R_{\text{carousel}} \cdot \sin(\Delta \theta_i)$$
    $$Z_i = -R_{\text{carousel}} \cdot (1 - \cos(\Delta \theta_i)) - Z_{\text{recession}}$$
    $$\text{Yaw}_i = -\Delta \theta_i$$
  - Opacity: Settles at `gleditor::anim::backgroundOpacity` ($0.42$) with depth fog.

### 2.2 Smooth Tab Transitions
When navigating tabs via `Ctrl+Tab`, `Ctrl+Shift+Tab`, or clicking a peripheral document quad:
1. `Choreograph` timeline interpolates document positions over $T = 0.42\,\text{s}$ using cubic ease-in-out.
2. The incoming document glides forward along $+Z$ into focus, rotating into flat alignment with the screen.
3. The outgoing document recedes smoothly along $-Z$ into its peripheral slot.

---

## 3. Floating 3D Word-Processing & Operational Controls

### 3.1 3D HUD Architecture
The word-processing and operational controls hover directly above the active document quad in 3D world space.

- **Coordinate Anchor**:
  $$\mathbf{P}_{\text{toolbar}} = \mathbf{P}_{\text{doc}} + \begin{pmatrix} 0 \\ \frac{1}{2} H_{\text{page}} + \text{margin}_y \\ \Delta z_{\text{elevation}} \end{pmatrix}$$
  where $\Delta z_{\text{elevation}} = 12.0\,\text{units}$ keeps the toolbar floating slightly in front of the page to eliminate z-fighting and cast a subtle shadow on the document header.

- **Visual Styling**:
  - Translucent rounded rect quad rendered via `gleditor::Canvas`.
  - Background: Frosted dark acrylic (RGBA `0.11, 0.13, 0.17, 0.88`).
  - Subtle top edge highlight (RGBA `0.45, 0.50, 0.65, 0.40`).
  - Drop shadow offset on $+Z$ and $-Y$.

### 3.2 Control Tool Matrix

| Segment | Icon | Action / Command | Shortcut |
| :--- | :--- | :--- | :--- |
| **File Operations** | `[+]` | `RenderItemNewDoc()` | `Ctrl+N` |
| | `[📂]` | `RenderItemOpenDoc()` | `Ctrl+O` |
| | `[💾]` | `RenderItemSaveDoc(active)` | `Ctrl+S` |
| | `[✕]` | `RenderItemCloseDoc(active)` | `Ctrl+W` |
| **View Modes** | `[⊞]` | Toggle 3D Carousel Overview | `F10` |
| | `[◫]` | Toggle Split Spatial View | `Ctrl+\` |
| **Formatting** | `[ B ]` | Toggle Bold (`SpanStyle::weight = 700`) | `Ctrl+B` |
| | `[ I ]` | Toggle Italic (`SpanStyle::slant = true`) | `Ctrl+I` |
| | `[ U ]` | Toggle Underline (`SpanStyle::underline = true`) | `Ctrl+U` |
| | `[ S ]` | Toggle Strikethrough (`SpanStyle::strike = true`) | `Ctrl+Shift+X` |
| **Typography** | `[ A⁻ ] [ A⁺ ]` | Adjust Font Scale ($0.8\times \leftrightarrow 2.0\times$) | `Ctrl+-` / `Ctrl+=` |
| | `[ H1 ] [ H2 ]` | Set Heading Hierarchy Level | `Ctrl+1` / `Ctrl+2` |
| **Paragraph** | `[ ≡ₗ ] [ ≡꜀ ] [ ≡ᵣ ]` | Align Left / Center / Right | `Ctrl+L` / `Ctrl+E` / `Ctrl+R` |
| **Lists** | `[ •- ] [ 1. ]` | Insert Bullet / Numbered List Item | `Ctrl+Shift+8` / `7` |

---

## 4. Picking, Interaction & Accessibility

### 4.1 3D Ray-Casting (`PickObserver`)
- Clicks and hovers are ray-cast from screen space $(x, y)$ through the inverse view-projection matrix $\mathbf{M}^{-1}_{\text{VP}}$ into 3D world space.
- Hit testing against toolbar button quads updates visual hover states and dispatches action callbacks on mouse down.

### 4.2 Accessibility (`a11y::Source`)
- The floating toolbar is registered with `a11y::Builder` as a `Role::Toolbar` container.
- Each button registers as an accessible `Role::Button` node with accessible name, state, and shortcut accelerators.
