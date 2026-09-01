# Xudu Intertwingled UI Design Workflow & 3-Way Tension Specification

## 1. Executive Summary

This specification establishes the comprehensive UI/UX architecture and agentic workflow for `apps/xudu` — Ted Nelson's vision of an open xanadoc and xanalogical universe where documents, raw primedia sources, and link packages intertwingle without artificial isolation walls.

```
       [ Parent Source A ]  <-- (Tenuous tether ribbons maintain origin connection)
          \           /
           \         /  (Smooth Flying Arc)
            v       v
      +-------------------+      Active Link Beam (Luminous Core Glow)      +-------------------+
      |  Active Xanadoc   |================================================>|  Transcluded Doc  |
      |  (Collinear X/Y)  |                                                 |  (Linked Page)    |
      +-------------------+                                                 +-------------------+
```

---

## 2. Wall-less Intertwingle Concept & Ambient Provenance

### 2.1 Wall-less Shared 3D Cosmos
- Traditional editors sequester text into isolated windows and modal files. In `xudu`, all xanadocs, transcluded source passages, and link packages inhabit a unified continuous 3D coordinate space.
- Documents and sources do not need explicit "File -> Open" actions to exist; they materialize automatically as they are referenced or transcluded by open views.

### 2.2 Unobtrusive Provenance & Active Indicators
- **Minimal Indicator**: Heavy titlebars and window chrome are eliminated. The active document/source is indicated by:
  - An ambient glowing boundary aura (soft 2px accent gradient with low-frequency breathing pulse).
  - A compact top-edge provenance tag displaying the microversion and spool hash (e.g. `OSMIC@v4:7e2b`).
- **Edit Tracking**:
  - Each edit is tagged in the underlying `Store` with its microversion op sequence.
  - In the UI, newly typed text is distinguished by author tinting, which fades over time to the default text color once blessed/committed into the immutable spool.

---

## 3. The 3-Way Tension Layout Engine

Documents and sources in `xudu` are positioned by a real-time continuous physics optimization solver balancing three competing priorities:

```
                          [ 1. Text Readability ]
                                   /\
                                  /  \
                                 /    \
                                /  ⚛   \
    [ 2. Link Collinear Alignment ] ---- [ 3. Global Aesthetic Harmony ]
```

### 3.1 Mathematical Energy Formulation
The total layout energy is defined as:
$$E_{\text{total}} = w_{\text{read}} E_{\text{read}} + w_{\text{align}} E_{\text{align}} + w_{\text{aest}} E_{\text{aest}}$$

#### Priority 1: Text Readability ($E_{\text{read}}$)
- Preserves comfortable viewing angles, orthogonal orientation to the camera view vector, and prevents document overlapping:
  $$E_{\text{read}} = \sum_i \frac{1}{2} k_{\text{cam}} \|\mathbf{P}_i - \mathbf{P}_{\text{cam\_opt}}\|^2 + \sum_{i \neq j} \frac{q_{\text{repel}}}{\|\mathbf{P}_i - \mathbf{P}_j\|^2 + \epsilon_{\text{read}}}$$

#### Priority 2: Link Collinear Alignment & Smooth Flying Pages ($E_{\text{align}}$)
- When a link is clicked, hovered, or active, the destination page smoothly flies out of its resting plane into side-by-side collinear alignment ($Y_i \approx Y_j, X_j \approx X_i + W_i + \text{gap}$):
  $$E_{\text{align}} = \sum_{(i, j) \in \text{Links}} \text{Prominence}(i, j) \cdot \left[ k_y (Y_i - Y_j)^2 + k_x (X_j - X_i - D_{\text{opt}})^2 + k_z (Z_i - Z_j)^2 \right]$$
- **Tenuous Connection to Parent Background**: When a linked page flies forward to align with the active focus, it remains anchored to its parent document in the background via a faint, semi-transparent elastic tether ribbon (quadratic Bezier arc with $\alpha \approx 0.25$).
- **Active Link Prominence**: The active link beam is rendered with full opacity, luminous core glow, and animated directional gradient energy pulse.

#### Priority 3: Global Link & Document Aesthetics ($E_{\text{aest}}$)
- Minimizes ribbon crossings and maintains clear depth separation between foreground active documents ($Z = 0$) and background reference corpora ($Z = -40\,\text{units}$):
  $$E_{\text{aest}} = \sum_{\text{edges } e_1, e_2} w_{\text{cross}} \cdot \text{CrossPenalty}(e_1, e_2) + \sum_i \frac{1}{2} k_{\text{layer}} (Z_i - Z_{\text{tier}(i)})^2$$

---

## 4. Visual Shaders & Animation Timing

- **Link Beams Pipeline (`gleditor::Beams`)**:
  - Vertex shader builds ribbons with variable thickness and orientation.
  - Fragment shader computes distance-to-edge glow and directional gradient fade:
    $$\text{Glow}(u) = \exp\left(-\frac{(u - 0.5)^2}{2 \sigma^2}\right)$$
- **Animation Timings (`gleditor::anim`)**:
  - `sworphSubject` = 0.62s (flying page leads movement).
  - `sworphRow` = 0.45s (background rows make way).
  - `backgroundOpacity` = 0.42 (ambient depth cue).
