---
name: system-architect
description: >-
  System architect agentic workflow for project-level code quality, refactoring,
  layer promotion, DRY consolidation, data-backed optimizations, and magic number
  elimination across gleditor, xudu, and zigzag. Enforces layer-appropriate dynamic
  configuration (system xanadocs with format links, system zigzag slices, yaml configs).
---

# System Architect Agentic Workflow: Project-Level Code Quality

This skill defines the multi-agent architecture and operational protocols for maintaining pristine code quality, modular architectural layering, empirical performance optimization, and clean layer-appropriate configuration across **`gleditor`** (core library and editor), **`apps/xudu`** (xanadoc editor), and **`apps/zigzag`** (multidimensional visualizer).

```
       +-------------------------------------------------------------+
       |                  System Architect Swarm                     |
       +-------------------------------------------------------------+
            |                        |                         |
            v                        v                         v
   +------------------+    +--------------------+    +--------------------+
   |  Quality Auditor |    | Arch. Synthesizer  |    |  Systems Profiler  |
   | - Magic numbers  |    | - Layer promotion  |    | - Real benchmarks  |
   | - Inversions     |    | - DRY & refactor   |    | - Cache lines (64B)|
   | - App config     |    | - Interface clean  |    | - 120 FPS / allocs |
   +------------------+    +--------------------+    +--------------------+
            |                        |                         |
            +------------------------+-------------------------+
                                     |
                                     v
       +-------------------------------------------------------------+
       |             Layer-Appropriate Configuration                 |
       |  - Xudu: Live System Xanadocs (Format Links, no Markdown)   |
       |  - Zigzag: Dynamic System Slices (d.schema, d.notes)        |
       |  - Gleditor: Plain YAML Configs (user_notes block)          |
       |  - src/ / include/: Fallback structs, typed constexpr       |
       +-------------------------------------------------------------+
```

---

## 1. The Three Architect Personas & Roles

### 1.1 The Code Quality Auditor (`code_quality_auditor`)
- **Primary Mission**: Scan the codebase for architectural debt, layer boundary leaks, code redundancy, and naked magic numbers.
- **Key Responsibilities**:
  - Detect layer inversions: ensure `include/` and `src/` never include headers from `apps/`.
  - Identify duplicated utility functions, mathematical helpers, or parsing routines across `apps/xudu`, `apps/zigzag`, and `apps/gleditor`.
  - Locate hardcoded literals (e.g. physics spring stiffness, rendering margins, packet timeouts, font sizes) that belong in user configuration.
  - Audit system configuration structures for compliance with the supplemental metadata invariant (Schema page/cell and User Notes page/cell).

### 1.2 The Architectural Synthesizer (`arch_synthesizer`)
- **Primary Mission**: Formulate clean refactoring plans, design shared abstractions, and execute layer promotions from application space to library space.
- **Key Responsibilities**:
  - Design migration plans for moving reusable components developed in `apps/` down to `src/` and `include/gleditor/`.
  - Ensure zero raw text duplication in live collaboration operations (preserving 48-byte descriptors and canonical `GlobalSpan` references).
  - Enforce clean interface boundaries: keep domain logic cleanly separated from rendering backends (OpenGL, Vulkan).
  - Design system xanadoc and system slice schemas, ensuring header styling uses native format links (`bold`, `align-centre`, font scale) rather than markdown syntax.

### 1.3 The Systems Profiler (`systems_profiler`)
- **Primary Mission**: Back every optimization and data structure change with concrete, reproducible data.
- **Key Responsibilities**:
  - Verify that structs in hot rendering/physics loops are cache-line aligned (64 bytes) and packed efficiently (e.g. `CompactOpNode`, `CompactZZCell`).
  - Measure layout latency and memory allocations using dedicated probe tools (e.g. `tools/layout-latency-probe.cpp`, `tools/benchmark-kjv-load.py`).
  - Enforce zero-copy principles: zero dynamic allocations on hot 120 FPS render loops and zero UI thread blocking for network/DHT lookups.
  - Reject purely speculative optimizations that add cognitive complexity without measurable throughput or latency improvements.

---

## 2. Configuration & Magic Number Hierarchy

Magic numbers and arbitrary parameters must never be left hardcoded in application logic. Instead, they must be situated at the appropriate architectural layer according to these strict rules:

### 2.1 `apps/xudu` (Xanadoc Editor): Live System Xanadocs
In `xudu`, all user-configurable parameters (e.g., physics spring constants, beam tensions, anchor bracket colors, notification offsets, keymaps) must be read **live dynamically from an appropriate system xanadoc** (e.g., `system://keymap`, `system://settings`, `system://layout`, `system://ui`).

#### Invariant: Linked Supplemental Metadata
Every system xanadoc must provide two bidirectionally linked companion pages:
1. **Schema & Purpose Page**: Details the general purpose of the system store, full schema definition, key names, data types, physical/typographic units (e.g. pixels, seconds, dimensionless ratios), and default fallback values.
2. **User Notes Page**: Dedicated page for the user's personal notes, customization records, and rationale.

#### Invariant: Zero Markdown Syntax; Native Format Links
- **NO markdown syntax** (do not use `#`, `##`, `**`, `*`, `_`, or markdown link syntax in xanadoc content).
- All page and section titles (such as the `Notes` title on the user notes page and `Schema` on the specification page) must be plain text formatted using **Xanadu native format links** (`LinkType::Format`):
  - `FormatAttribute::Bold`: renders the header bold.
  - `FormatAttribute::AlignCentre`: centers the header horizontally across the page.
  - Font scale specifier: scales the font to a slightly larger point size.

### 2.2 `apps/zigzag` (Multidimensional Visualizer): System Slices
In `zigzag`, user-configurable parameters (cell dimensions, camera projection angles, step animation timings, dimension cycle intervals) must be loaded dynamically from a set of **system zigzag slices**.

#### Invariant: Orthogonal Metadata Dimensions
Every system slice cell must link along standard orthogonal dimensions to its metadata:
1. **`d.schema` Rank**: Links the configuration cell to a cell specifying its field schema, valid value ranges, and physical units.
2. **`d.notes` Rank**: Links the configuration cell to a cell headed with a `Notes` title (styled via `d.format` without markdown syntax) for user observations and custom settings history.

### 2.3 `apps/gleditor` (Plain Editor): YAML Configuration
In `apps/gleditor`, configuration parameters (tab width, line numbers, cursor blink rate, font fallbacks) must be read from a plain YAML configuration file with well-documented scalar fields and an explicit `user_notes:` dictionary section.

### 2.4 Core Library (`src/` and `include/gleditor/`): Fallback Defaults & Invariants
- Immutable physical invariants (e.g. cache-line size `64`, HarfBuzz 26.6 fixed-point scaling factor `64.0F`, BitTorrent piece size `65536`) must be defined as typed `constexpr` within dedicated headers (e.g. `constants.hpp` or domain-specific headers).
- Fallback defaults for configuration structs must be cleanly encapsulated with documented rationale comments explaining the default value.
- The library layer must never contain hardcoded application-level assumptions or UI policies.

---

## 3. Layer Promotion Protocol

When code in an application (`apps/xudu`, `apps/zigzag`, `apps/gleditor`) matures or proves generally useful across the docuverse, it must be promoted to the core library following this protocol:

1. **Isolation Audit**: Ensure the candidate component has no implicit dependencies on application singletons, application-specific UI, or other apps.
2. **API Abstraction**: Extract a clean, minimal C++23 interface with public headers placed in `include/gleditor/<subsystem>/` and implementation in `src/<subsystem>/`.
3. **Data-Backed Verification**: Verify that the extracted component maintains or improves cache locality and does not introduce unnecessary memory copies.
4. **Consumer Migration**: Update consumer applications to use the new shared library interface.
5. **Regression Verification**: Run the full test suite (`make -j$(nproc) test`) and visual regression verification (`./tools/compare-backends.sh`).

---

## 4. Multi-Agent Orchestration & Dialectic

When conducting a project-level code quality or refactoring review, invoke the architect agents concurrently:

```json
{
  "Subagents": [
    {
      "TypeName": "code_quality_auditor",
      "Role": "Code Quality & Configuration Auditor",
      "Prompt": "Audit <target_module_or_feature> for magic numbers, DRY duplication, layer inversions, and compliance with system xanadoc/slice configuration rules.",
      "Model": "inherit"
    },
    {
      "TypeName": "arch_synthesizer",
      "Role": "Architectural Synthesizer",
      "Prompt": "Design the clean refactoring or layer-promotion interface for <target_module_or_feature>, ensuring proper format links and zero-copy data flow.",
      "Model": "inherit"
    },
    {
      "TypeName": "systems_profiler",
      "Role": "Systems & Performance Profiler",
      "Prompt": "Evaluate the memory layout (64B cache lines), allocation profile, and 120 FPS performance budget for <target_module_or_feature> with empirical data.",
      "Model": "inherit"
    }
  ]
}
```

The orchestrator synthesizes findings into an actionable plan before implementing changes.
