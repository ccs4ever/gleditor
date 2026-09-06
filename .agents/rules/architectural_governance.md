# Architectural Governance & Code Quality Rules

This rule enforces structural integrity, clean layering, dynamic layer-appropriate configuration, zero naked magic numbers, and data-backed performance across `gleditor`, `apps/xudu`, and `apps/zigzag`.

## 1. Architectural Layering & Dependency Hierarchy

- **Library Purity**: `include/gleditor/` and `src/` must **never** include or depend on any code in `apps/` (`apps/gleditor`, `apps/xudu`, `apps/zigzag`).
- **Application Isolation**: Applications may consume the core library (`include/gleditor/`), but must **never** cross-include or depend on each other (`apps/xudu` cannot include `apps/zigzag`, and vice-versa).
- **Promotion Discipline**: Reusable utilities, data structures, or algorithms must be moved to `src/` rather than copied between apps.

## 2. Dynamic Configuration & Magic Number Elimination

Naked numerical literals and hardcoded behavioral magic numbers are strictly forbidden in application algorithms, layout routines, physics, and networking.

- **In `apps/xudu`**:
  - All user-configurable values (physics spring parameters, beam tensions, link anchor colors, notification offsets, keymaps) must be read **live dynamically from an appropriate system xanadoc** (`system://...`).
  - **Zero Markdown Syntax**: System xanadocs and their companion documents must NOT use markdown syntax (no `#`, `**`, etc.).
  - **Native Format Links**: Page and section headers (such as `Notes` and `Schema`) must be styled with native Xanadu format links (`FormatAttribute::Bold`, `FormatAttribute::AlignCentre`, and font scale specifiers).
  - **Required Supplemental Metadata**: Every system xanadoc must be bidirectionally xanalinked to:
    1. A **Schema & Purpose Page** documenting the store's role, fields, data types, physical units, and fallback defaults.
    2. A dedicated **User Notes Page** headed with `Notes` (formatted with bold, centered, larger format links) for user overrides and thoughts.
- **In `apps/zigzag`**:
  - Configurable parameters (cell spacing, camera projections, cycler speeds) must be resolved dynamically from **system zigzag slices**.
  - No markdown syntax in cells; metadata must link along orthogonal dimensions: `d.schema` (purpose and schema) and `d.notes` (user notes).
- **In `apps/gleditor`**:
  - Configurable parameters must be loaded from a **plain YAML configuration file** with descriptive comments and a `user_notes:` section.
- **In `src/` and `include/gleditor/`**:
  - Core library fallback defaults and immutable physical invariants (e.g. cache-line size 64B, HarfBuzz 26.6 scale factor) must be explicitly typed `constexpr` within dedicated domain constants headers with rationale comments.

## 3. Systems Realism & Empirical Verification

- Hot rendering loops (120 FPS target / 8.33ms budget) must have **zero dynamic memory allocations**.
- Critical hot structs must be 64-byte aligned and cache-friendly (e.g. `CompactOpNode`, `CompactZZCell`).
- Performance claims and optimizations must be supported by empirical data using probe tools (`tools/layout-latency-probe.cpp`, `tools/benchmark-kjv-load.py`).
