---
name: xudu-ui-design
description: >-
  Expert workflow and runbook for comprehensive UI design in the xudu xanadoc editor.
  Use when designing or implementing wall-less intertwingled layouts, 3-way tension spring layout physics, tenuous link tethers, flying linked pages, transclusion rendering, and provenance tracking.
---

# `xudu` Intertwingled UI Design Workflow

This skill defines the technical procedures, physical force models, transclusion rendering conventions, and layout algorithms for the `xudu` xanadoc and xanalogical editor.

## 1. Core Architectural Concept

`xudu` does not place artificial walls between xanadocs, primedia sources, and link packages. All information entities intertwingle in a shared 3D cosmos:
- **Reference-Driven Emergence**: Documents, source spans, and link targets materialize in 3D space as they are referenced or transcluded.
- **Unobtrusive Provenance & State**: The active document or source is marked with minimal ambient cues (soft edge aura, subtle header tag with author/microversion) rather than heavy window chrome.
- **Edit Tracking**: Edits are tagged with their origin spool, microversion signature, and xanadoc context.

---

## 2. The 3-Way Tension Layout Engine

Every document and transcluded source is subject to three competing dynamic forces in a unified physics simulation:

### Force 1: Text Readability ($F_{\text{read}}$)
- **Objective**: Maintain optimal font readability, comfortable visual field of view, face normal towards camera ($\hat{\mathbf{N}} \approx -\hat{\mathbf{Z}}$), and prevent text occlusion.
- **Energy Term**:
  $$E_{\text{read}} = \frac{1}{2} k_{\text{cam}} \|\mathbf{P}_i - \mathbf{P}_{\text{opt}}\|^2 + \frac{1}{2} k_{\text{orient}} (1 - \hat{\mathbf{N}}_i \cdot \hat{\mathbf{V}}_{\text{cam}})^2 + \sum_{j \neq i} \frac{q_{\text{occlude}}}{\|\mathbf{P}_i - \mathbf{P}_j\|^2 + \epsilon}$$

### Force 2: Link Collinear Alignment ($F_{\text{align}}$)
- **Objective**: When a link is selected or hovered, the destination document or passage flies smoothly into collinear horizontal proximity ($Y_A \approx Y_B$, $X_B \approx X_A + W_A + \text{gap}$) so that both ends can be read side-by-side without eye strain.
- **Tenuous Connection to Parent**: As a linked page flies forward to align with the active focus, it remains anchored to its parent document in the background via a faint, semi-transparent elastic tether ribbon (quadratic Bezier arc with $\alpha \approx 0.25$).
- **Active Link Prominence**: The active link beam is rendered with full opacity, luminous core glow, and animated directional gradient energy pulse.

### Force 3: Global Link & Document Aesthetics ($F_{\text{aest}}$)
- **Objective**: Prevent ribbon entanglement, avoid crossings, maintain harmonic depth layering ($Z_{\text{active}} = 0, Z_{\text{background}} = -40\,\text{px}$), and apply background dimming (`backgroundOpacity = 0.42`).
- **Energy Term**:
  $$E_{\text{aest}} = \sum_{\text{beams } p, q} w_{\text{cross}} \cdot \text{IntersectionPenalty}(p, q) + \frac{1}{2} k_{\text{layer}} (Z_i - Z_{\text{tier}(i)})^2$$

---

## 3. Visual & Shader Design

### Link Beam & Tether Rendering (`gleditor::Beams`)
- **Active Link Beam**: Luminous gradient shader along the span with pulsing phase offset $\phi(t) = \omega t$. Color mapped by `LinkType` (Comment: cyan/blue, Citation: purple/magenta, Transclusion: gold/amber).
- **Tenuous Parent Tether**: Subtle Bezier ribbon connecting flying passage back to its origin slot in the parent document background:
  $$\mathbf{B}(t) = (1-t)^2 \mathbf{P}_{\text{parent}} + 2(1-t)t \mathbf{P}_{\text{control}} + t^2 \mathbf{P}_{\text{flying}}, \quad t \in [0, 1]$$
  where $\mathbf{P}_{\text{control}} = \frac{\mathbf{P}_{\text{parent}} + \mathbf{P}_{\text{flying}}}{2} + \begin{pmatrix} 0 \\ 0 \\ -15 \end{pmatrix}$.

### Provenance Highlighting (`SpanDecorator`)
- Spans transcluded from other spools or documents carry a thin 2px left margin accent bar and gentle background tint according to author/spool hash color.
- Microversion pills hover unobtrusively above each document block showing `doc@v3:a9f1`.

---

## 4. Implementation Checklist for New Features

1. **Tension Solver Module**: Implement in `apps/xudu/core/tension_layout.cpp` integrating Runge-Kutta 4th-order (RK4) spring simulation.
2. **Link Beams Integration**: Extend `apps/xudu/beams.cpp` with tether curve generation and active link pulsating shaders.
3. **Session Observation**: Update `apps/xudu/session.cpp` to monitor caret position, link hovering, and dynamic document spawning.
4. **Accessibility Mapping**: Update `xudu::LinkBeams::describe` in `apps/xudu/beams.cpp` to expose link relations and provenance metadata.
