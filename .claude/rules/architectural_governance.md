# Architectural Governance & Code Quality Rules

This rule enforces structural integrity, clean layering, dynamic layer-appropriate configuration,
zero naked magic numbers, and data-backed performance across the `gleditor` library, the plain
`gleditor` editor, and `xuzz`.

## 1. Architectural Layering & Dependency Hierarchy

- **A Generic Library**: `libgleditor` (`include/gleditor/`, `src/`) holds generic components only:
  the backbone that `xuzz` and others specialise. `apps/gleditor` uses it and has no Xanadu
  reference by design, so no library type, file or test is named for a cell, a link, a xanadoc, a
  slice or a view kind, and the library must **never** include or depend on any code in `apps/`.
- **One Xanadu/ZigZag Application**: `xuzz` is the only program for xanadocs and ZigZag slices.
  `xudu` and `zigzag` are retired as applications (`build/xudu` and `build/zigzag` are symlinks to
  `xuzz`).
- **Where Code Goes**:
  - Everything xanalogical goes in `apps/common/`: `apps/common/xanadu/` (namespace `xanadu`) for
    code that needs no graphics device, so `vquery`, `vqueryc`, `vpl`, `vplc` and `vprolog` can
    share it; `apps/common/ui/` for code that draws or takes input through the library.
  - `apps/xuzz/` holds only what is truly unique to the xuzz program — `main.cpp`, the command line,
    the application wiring. That should be rare.
  - **Nothing lives in `apps/xudu/` or `apps/zigzag/`.** Add no file there. The code still in them
    is to be moved to the places above (`design/view-system.md` §17 step 1 has the table); any
    change that touches one of those files should move it.
- **Engine Purity**: `apps/common/xanadu/` must **never** include `apps/xuzz/`, `apps/common/ui/` or
  a library header that needs a graphics device. Header-only, device-free library headers
  (`<gleditor/cpp26*.hpp>`, `<gleditor/spatial.hpp>`, `<gleditor/draw_budget.hpp>`) are allowed.
- **Plain Editor Isolation**: `apps/gleditor` consumes the core library only and must share no code
  or dependency with Xanadu, ZigZag or Xuzz.
- **Tests Sit With Their Code, Once**: the library has its own battery in `tests/lib/`
  (`gleditor_test`). `xuzz_test` is kept to xuzz's own code and links no library: it must not repeat
  a library test, and a test of xanalogical code takes library behaviour (text fitting, focus,
  projection, clipping) as given instead of sweeping it again.
- **Promotion Discipline**: Reusable utilities, data structures, or algorithms must be moved down
  (to the engine, or to the library when they are generic) rather than copied.

## 2. Dynamic Configuration & Magic Number Elimination

Naked numerical literals and hardcoded behavioral magic numbers are strictly forbidden in
application algorithms, layout routines, physics, and networking.

- **Xanadocs in `xuzz`**:
  - All user-configurable values (physics spring parameters, beam tensions, link anchor colors,
    notification offsets, keymaps) must be read **live dynamically from an appropriate system
    xanadoc** (`system://...`).
  - **Zero Markdown Syntax**: System xanadocs and their companion documents must NOT use markdown
    syntax (no `#`, `**`, etc.).
  - **Native Format Links**: Page and section headers (such as `Notes` and `Schema`) must be styled
    with native Xanadu format links (`FormatAttribute::Bold`, `FormatAttribute::AlignCentre`, and
    font scale specifiers).
  - **Required Supplemental Metadata**: Every system xanadoc must be bidirectionally xanalinked to:
    1. A **Schema & Purpose Page** documenting the store's role, fields, data types, physical units,
       and fallback defaults.
    1. A dedicated **User Notes Page** headed with `Notes` (formatted with bold, centered, larger
       format links) for user overrides and thoughts.
- **ZigZag slices in `xuzz`**:
  - **Zigzag Store Invariant**: Zigzag documents and multidimensional spaces must always be backed
    by a sovereign `xanadu::Store` (with `CompactOpNode`, `OpKind::Structure`, and
    `Store::rebuildManifold()`), never legacy YAML slice files (`.yaml`). The legacy YAML slice
    format is deprecated in favor of stores.
  - Configurable parameters (cell spacing, camera projections, cycler speeds) must be resolved
    dynamically from **system zigzag slices** backed by sovereign stores.
  - No markdown syntax in cells; metadata must link along orthogonal dimensions: `d.schema` (purpose
    and schema) and `d.notes` (user notes).
- **In `apps/gleditor`**:
  - Configurable parameters must be loaded from a **plain YAML configuration file** with descriptive
    comments and a `user_notes:` section.
- **In `src/` and `include/gleditor/`**:
  - Core library fallback defaults and immutable physical invariants (e.g. cache-line size 64B,
    HarfBuzz 26.6 scale factor) must be explicitly typed `constexpr` within dedicated domain
    constants headers with rationale comments.

## 3. Systems Realism & Empirical Verification

- Hot rendering loops must have **zero dynamic memory allocations**.
- Critical hot structs must be 64-byte aligned and cache-friendly (e.g. `CompactOpNode`,
  `CompactZZCell`).
- Performance claims and optimizations must be supported by empirical data using probe tools
  (`tools/layout-latency-probe.cpp`, `tools/benchmark-kjv-load.py`).
- This system is not in production yet, no need to preserve backwards compatibility of any format
  except when other constraints are involved such as alignment and cache line sizing.

## 4. Sovereign Keymap & Vortex Hyperstructural Governance

- **Sovereign Keymap Storage**: All key bindings in `xuzz` must be defined in the `system://keymap`
  system store, never hardcoded in C++ application code. Key binding actions must use Vortex
  function calls (or registered Vortex routines/macros) for their actions. The `gleditor`
  application is explicitly exempt: `apps/gleditor` is the plain editor that must not share any code
  or dependency with Zigzag, Xanadu, or Xuzz, and retains its own independent YAML configuration.
- **C++ Implementation Justification**: New C++ code must justify why it isn't being written in
  Vortex (e.g., hardware/driver interfacing, low-level rendering intrinsics, memory allocator
  primitives, or raw OS event handling).
- **Vortex Standard Library Reuse**: New Vortex standard library code written in Vortex must
  leverage existing Vortex standard library functions unless absolutely necessary.
