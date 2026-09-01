---
name: gleditor-ui-design
description: >-
  Expert workflow and runbook for comprehensive 3D UI design in the gleditor text editor.
  Use when designing or implementing 3D document spaces, spatial tab switching, floating 3D word-processing and operational controls, and rendering integration in gleditor.
---

# `gleditor` 3D UI Design Workflow

This skill defines the technical procedures, mathematical coordinate models, interaction mechanics, and rendering integration for the `gleditor` 3D text editor application.

## 1. Core Architectural Concept

`gleditor` elevates the traditional desktop document editor into 3D world space:
- **Spatial Document Separation**: Each document exists on its own distinct 3D spatial plane (carousel, cylindrical orbit, or stacked spatial desk slots) rather than 2D flat tabs.
- **Tab Switching as 3D Camera/Plane Choreography**: Switching tabs translates and rotates the active document into primary focus ($Z = 0$, orthogonal to camera) while moving other open documents into peripheral background orbits ($Z < 0$ or curved arcs with dimmed opacity).
- **Floating 3D Word-Processing HUD**: Formatting and operational toolbars hover directly in 3D world coordinates above the active document's top margin, facing the viewer with subtle depth parallax and frosted glass quad backing.

---

## 2. Floating 3D Control Bar (HUD) Specifications

### Geometry & World Anchor
- **Positioning**: Anchored to the active document's world transform:
  $$\mathbf{P}_{\text{toolbar}} = \mathbf{P}_{\text{doc}} + \begin{pmatrix} 0 \\ H_{\text{page}}/2 + \Delta y_{\text{margin}} \\ \Delta z_{\text{elevation}} \end{pmatrix}$$
  where $\Delta y_{\text{margin}} \approx 28.0\,\text{px}$, $\Delta z_{\text{elevation}} \approx 12.0\,\text{px}$ (slight forward elevation towards camera).
- **Backing Plate**: Rendered via `Canvas` as a rounded rectangle with frosted translucency:
  - Fill: RGBA `(0.12, 0.14, 0.18, 0.88)` with subtle specular border `(0.3, 0.35, 0.45, 0.60)`.
  - Drop shadow offset along $-Y$ and $+Z$.

### Control Item Groups

| Group | Controls | Visual Icon / Glyph | Action / State Mapping |
| :--- | :--- | :--- | :--- |
| **Operational** | New Doc, Open, Save, Save As, Close Tab | `[+]`, `[📂]`, `[💾]`, `[💾+]`, `[✕]` | Dispatches `RenderItemNewDoc`, `RenderItemSaveDoc`, `RenderItemCloseDoc` |
| **View / Layout** | 3D Overview, Split View, Zoom Fit | `[⊞]`, `[◫]`, `[⛶]` | Toggles camera orbit overview / dual document space |
| **Text Styling** | Bold, Italic, Underline, Strikethrough | `[ B ]`, `[ I ]`, `[ U ]`, `[ S ]` | Toggles font weight/slant flags in `SpanStyle` |
| **Typography** | Font Size (−/+), Heading Level (P, H1, H2, H3) | `[ A⁻ ] [ A⁺ ]`, `[ H1 ] [ H2 ]` | Updates font scale multiplier and line metrics |
| **Paragraph** | Align Left, Center, Right, Justify | `[ ≡ₗ ]`, `[ ≡꜀ ]`, `[ ≡ᵣ ]`, `[ ≡ⱼ ]` | Adjusts layout line alignment in `TextLayout` |
| **Blocks & Lists** | Bullet List, Numbered List, Code Block, Quote | `[ •- ]`, `[ 1. ]`, `[ </> ]`, `[ " ]` | Inserts structured prefix / formatting block |

---

## 3. Spatial Tab Switching & Camera Choreography

### Document 3D Slot Formulations

For $N$ open documents with active index $A$:
$$\theta_i = (i - A) \cdot \Delta \theta \quad \text{where } \Delta \theta = \frac{\pi}{6} \approx 30^\circ$$
$$\mathbf{X}_i = R \cdot \sin(\theta_i), \quad \mathbf{Z}_i = -R \cdot (1 - \cos(\theta_i))$$
$$\text{Rotation}_Y(i) = -\theta_i$$
$$\text{Opacity}(i) = \begin{cases} 1.0 & \text{if } i = A \\ \max(0.25, \cos(\theta_i) \cdot 0.45) & \text{if } i \neq A \end{cases}$$

### Easing & Timings
- Uses `Choreograph` timeline with cubic bezier / quad ease-in-out:
  - Tab transition duration: $T = 0.42\,\text{s}$.
  - Camera settle delay: $0.08\,\text{s}$.
  - Toolbar elevation hover tween: $0.18\,\text{s}$.

---

## 4. Interaction, Picking, and Accessibility

### 3D Ray Picking (`PickObserver`)
1. Camera projects screen mouse coordinates $(x, y)$ to world ray $\mathbf{R}(t) = \mathbf{O} + t \mathbf{D}$.
2. Intersects ray against the floating toolbar's oriented bounding box (OBB) and the document quad.
3. Maps hit coordinate to button tag index and triggers hover/click feedback.

### Accessibility (`a11y::Source`)
- Exposes toolbar as an accessible toolbar role node in `a11y::Builder`.
- Each button is an accessible `Button` node with name (e.g. "Bold (Ctrl+B)", "Save (Ctrl+S)") and keyboard shortcut accelerators.

---

## 5. Implementation Checklist for New Features

1. **Toolbar Component**: Implement in `src/ui/floating_toolbar.cpp` inheriting `FrameContributor`, `PickObserver`, `a11y::Source`.
2. **Document Spatial Manager**: Coordinate document transform matrices in `src/render_state.cpp`.
3. **Command Binding**: Wire keyboard shortcuts and toolbar events in `apps/gleditor/main.cpp`.
4. **Shader Pipeline**: Ensure quads render with depth test enabled and alpha blending in `render::PipelineDesc`.
