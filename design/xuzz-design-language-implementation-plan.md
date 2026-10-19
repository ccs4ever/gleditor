# Xuzz Design Language Implementation Plan

## 1. Overview & Architectural Scope

This plan specifies the phased engineering roadmap for implementing the **Xuzz Design Language**
defined in [`design/xuzz-design-language.md`](xuzz-design-language.md). It transforms the Xuzz user
interface from functional and bland into a polished, authentic Nelsonian hypermedium by:

1. Eliminating oversized, haphazard button blocks through adaptive sizing and purposeful button
   shapes.
1. Implementing mathematical optical label centering in the core canvas engine.
1. Streamlining the 3×3 grid link navigator into an ergonomic 38px single bar with bilateral endset
   wings.
1. Replacing raw concatext byte dumps with human-readable ambient semantic provenance.
1. Implementing the searchable, facet-filtered Link Directory Dialog.
1. Overhauling the Hypertime Branching DAG with vector microversion orbs, filleted orthogonal rails,
   a Vernier caliper timeline scrubber, and Cybernetic Vellum N-way diff cards.
1. Binding sovereign keymap chords in `system://keymap` and maintaining AccessKit parity.

______________________________________________________________________

## 2. File Inventory & Proposed Changes

### 2.1 Core Library (`gleditor`)

#### `include/gleditor/ui/theme.hpp`

- Add `ButtonMetrics` struct:
  - `heightCompactPx = 28.0F` (desktop toolbar/dialog controls)
  - `heightStandardPx = 32.0F` (desktop primary action verbs)
  - `heightTouchPx = 44.0F` (touch modality minimum)
  - `radiusPillShare = 0.5F` (capsule pill radius multiplier)
  - `radiusChamferPx = 4.0F` (chamfered tab corner cut)
- Add `XanaduFlairColours` struct defining the P5-invariant harmonic color bands:
  - `obsidianVoid`, `slateSurface`, `buttonPlate`, `wellRecessed`
  - `identityGold`, `celestialCyan`, `amethystPurple`, `emeraldMint`, `coralCrimson`
  - `goldAura`, `cyanAura`, `rimGlowHairline`

#### `src/canvas.cpp`

- Refactor `Canvas::addText(RenderState &, ui::Rect, const text::FittedText &, ...)` to compute
  exact optical centering:
  - Horizontal: `box.left + round((W - W_text) / 2)`
  - Vertical: `box.bottom + round((H + ascent + descent) / 2)`
  - Descenderless cap-height adjustment when text has zero descenders.

#### `src/ui/widgets.cpp`

- Update `WidgetBuilder::natural()`:
  - Replace unconditional `touch()` (44px) clamp with modality-aware button sizing.
  - Implement adaptive button width calculation based on fitted label width and padding.
  - Add support for segmented button grouping with hairline inner seams.

#### `src/ui/overlay.cpp`

- Enforce Principle P1:
  - Draw view-only chrome (buttons, chips, panels) with soft-cornered geometry
    (`r = 0.5 × theme.paddingEm`) or chamfered edges rather than raw hard quads.
  - Render recessed luminescent wells for text inputs with subtle inner crease and focus glow.

______________________________________________________________________

### 2.2 Navigation & Presentation Layer (`apps/common/ui/` & `apps/common/xanadu/`)

#### `apps/common/xanadu/link_panel.hpp` & `link_panel.cpp`

- Expel raw concatext byte offsets:
  - Replace `"document %u, bytes %u to %u"` with semantic passage representations.
  - Format passage excerpts with typographic ellipsis framing.
  - Format ZigZag targets with rank and cell labels (`cell 37 [echo] on d.link +1`).
- Re-architect button commands:
  - Replace 9-button flat list with structured tripartite commands:
    - Left wing: `StepMember{-1, Left}`, `StepMember{+1, Left}`, `StepOccurrence{-1, Left}`,
      `StepOccurrence{+1, Left}`
    - Center hub: `StepLink{-1}`, `StepLink{+1}`, `Cross`, `ExpandDirectory`
    - Right wing: `StepMember{-1, Right}`, `StepMember{+1, Right}`, `StepOccurrence{-1, Right}`,
      `StepOccurrence{+1, Right}`
    - Actions: `Enter`, `ReturnToOrigin`, `ActivityBack`, `Dismiss`

#### `apps/common/ui/link_panel_presentation.hpp` & `link_panel_presentation.cpp`

- Reconstruct layout from 3×3 `ButtonFlow` grid into a single horizontal strip (`height = 38px`):
  - Zone A (Left Wing): Segmented steppers for Left Member/Occurrence, passage snippet, status tag
    pill.
  - Zone B (Center Hub): Segmented Link stepper, semantic Link Type badge, curator pill, active side
    indicator, expand trigger.
  - Zone C (Right Wing): Symmetrical controls for Right Member/Occurrence.
  - Zone D (Action Dock): Cross, Enter, Origin, Activity Back, and Dismiss pills.

#### `apps/common/ui/xanadoc/link_panel_overlay.hpp` & `link_panel_overlay.cpp`

- Connect new segmented button and dock picking IDs.
- Support smooth docking/floating animation along the bottom safe area.
- Add trigger callback to launch the Expanded Link Directory Dialog.

#### `[NEW] apps/common/ui/link_directory_dialog.hpp` & `link_directory_dialog.cpp`

- Implement the 780px × 540px modal/docked link directory inspector:
  - Full-text search field filtering passages across both endsets.
  - Faceted semantic filter pills (Type, Author, Topology).
  - Two-pane layout: searchable link enfilade on left, full detail card on right.
  - Collapsible Advanced Primedia Inspector (exact primedia spans and storage hashes hidden by
    default).

______________________________________________________________________

### 2.3 Hypertime DAG & Visual Diff Engine (`apps/common/ui/hypertime_graph.*`)

#### `apps/common/ui/hypertime_graph.hpp` & `hypertime_graph.cpp`

- Vector-Quality Microversion Nodes:
  - Replace the 1px `addRect` loop in `drawDisc` with vector-quality SDF quad rendering.
  - Render 24px orbs with 3px lineage aura, 1.5px luminous ring, obsidian core, and centered
    monospace op glyphs (`G`, `I`, `D`, `R`, `T`, `L`, `S`).
- Orthogonal Lineage Rails:
  - Implement multi-pass lines with 4px diffusion aura and 4px filleted turns at branch elbows.
- Temporal Crystal Branch Nodes:
  - Render rotated diamond prisms ($45^\circ$) with dual-faceted specular shading at fork points.
- Tactile Vernier Caliper Scrubber:
  - Implement metallic bronze track groove with 2px commit density pips.
  - Add knurled carriage with live monospace readout and magnetic milestone snapping.
- Cybernetic Vellum N-Way Diff Cards:
  - Layered translucent cards with P5-adherent syntax highlighting:
    - Universal Invariant Spans: Identity Gold (`#FFD700`) wash + glowing baseline ribbon.
    - Shared Branch Spans: Amber (`#F59E0B`) wash.
    - Unique Additions: Mint Green (`#10B981`) wash.
    - Limbo Deletions: Coral Red (`#EF4444`) wash with strikethrough.
  - Implement the `[ ❝ Quote into Head ]` transclusion action.

______________________________________________________________________

## 3. Phased Implementation Roadmap

### Phase 1: Core Geometry, Adaptive Sizing & Optical Centering

- Extend `theme.hpp` with `ButtonMetrics` and `XanaduFlairColours`.
- Implement mathematical optical label centering in `Canvas::addText` (`src/canvas.cpp`).
- Update `WidgetBuilder::natural()` in `src/ui/widgets.cpp` to use adaptive button metrics.
- Enforce soft-cornered view chrome rendering in `src/ui/overlay.cpp`.
- Verify with `tests/lib/ui_widgets_test.cpp`.

### Phase 2: Streamlined Link Navigator Bar & Semantic Provenance

- Refactor `link_panel.cpp` to emit semantic passage anchors and structured tripartite commands.
- Refactor `link_panel_presentation.cpp` to layout the 38px single horizontal bar.
- Update `link_panel_overlay.cpp` to bind picking and drawing for the new zones.
- Verify with `tests/xuzz/link_panel_test.cpp` and `tests/zigzag/link_panel_overlay_test.cpp`.

### Phase 3: Expanded Link Directory Dialog

- Create `apps/common/ui/link_directory_dialog.hpp` and `.cpp`.
- Implement full-text passage search and faceted filter pills.
- Add collapsible advanced primedia disclosure.
- Wire dialog invocation from `link_panel_overlay.cpp` and `xuzz_app.cpp`.
- Write dedicated unit tests in `tests/xuzz/link_directory_dialog_test.cpp`.

### Phase 4: Hypertime DAG & Visual Diff HUD Overhaul

- Upgrade `HypertimeGraph::drawDisc` to vector/SDF quad rendering with lineage auras.
- Add filleted turns to branch rail polylines and temporal crystal branch nodes.
- Implement the Vernier caliper timeline scrubber with microversion commit pips.
- Implement Cybernetic Vellum diff cards with `[Quote into Head]` transclusion.
- Verify with `tests/zigzag/hypertime_graph_overlay_test.cpp`.

### Phase 5: Sovereign Keymap Chords & AccessKit Integration

- Register all chords (`Alt+Shift+N`, `Alt+Shift+P`, `Alt+Shift+J`, `Alt+Shift+K`, `Alt+Shift+L`,
  `Alt+Shift+H`, `Alt+Shift+X`, `Alt+Shift+Return`, `Alt+Shift+E`, `Alt+Shift+O`, `Alt+Shift+B`,
  `Ctrl+H`, `Ctrl+Shift+Q`) in `system://keymap`.
- Update AccessKit tree nodes across the link bar, dialog, and hypertime HUD.
- Verify keyboard-only and screen-reader accessibility navigation.

### Phase 6: Headless Verification, Backend Parity & Journey Audits

- Run unit test suites across `gleditor_test`, `xuzz_test`, `zigzag_test`.
- Run `./tools/compare-backends.sh` under `xvfb-run` to verify visual parity across OpenGL, GLES,
  and Vulkan.
- Audit real-work journeys J4, J10, J11, J12, J14, J15, J16.
- Run `make format-check` and `make lint`.

______________________________________________________________________

## 4. Verification Plan

### Automated Unit Tests

```sh
# Execute headless unit tests for widgets, link navigation, and hypertime DAG
make -j$(nproc) test TEST_FILTER='LinkPanelTest.*:LinkPanelOverlayTest.*:HypertimeGraphOverlayTest.*:UiWidgetsTest.*'
```

### Visual Parity Across Backends

```sh
# Compare rendering output across compiled-in backends under virtual X11
xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh
```

### Code Style & Linting Gates

```sh
make format-check
make lint
```

### Journey Validation Matrix (per `design/ux_workflow_real_work.md`)

- **J4:** Navigate home cell and back while Link Bar stays pinned without resetting view state.
- **J10:** Toggle views (`Both` ↔ `Slices only` ↔ `Xanadocs only`) with active link context
  preserved.
- **J11:** Browse 1:2 link with distant cells staged without Cartesian strand explosion.
- **J12:** Transclude cell into document and cell, validating matching primedia addresses in Link
  Bar.
- **J14:** Display typed value badges on ZigZag ranks.
- **J15:** Compare 3 branches at change sites using Cybernetic Vellum diff cards and
  `Quote into Head`.
- **J16:** Navigate crowded 2×3 branching link with complete endsets browsed independently.
