---
name: system-architect
description: >-
  System architect agentic workflow for project-level code quality, refactoring,
  layer promotion, DRY consolidation, data-backed optimizations, and magic number
  elimination across the gleditor library, the plain gleditor editor, and xuzz. Enforces layer-appropriate dynamic
  configuration (system xanadocs with format links, system zigzag slices, yaml configs).
---

# System Architect Agentic Workflow: Project-Level Code Quality

This skill defines the multi-agent architecture and operational protocols for maintaining pristine
code quality, modular architectural layering, empirical performance optimization, and clean
layer-appropriate configuration across **`gleditor`** (core library and plain editor) and
**`xuzz`**, the one application for xanadocs and ZigZag slices. `xudu` and `zigzag` are retired as
applications and there is no `apps/xudu/` or `apps/zigzag/`. The library stays generic (the plain
editor uses it and has no Xanadu reference); xanalogical code goes in `apps/common/` —
`apps/common/xanadu/` without a graphics device, `apps/common/ui/` with one — and `apps/xuzz/` holds
only what is unique to that program. Library tests live in `tests/lib/` and are not repeated in
`xuzz_test`.

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
   | - App config     |    | - Interface clean  |    | - render path / allocs |
   +------------------+    +--------------------+    +--------------------+
            |                        |                         |
            +------------------------+-------------------------+
                                     |
                                     v
       +-------------------------------------------------------------+
       |             Layer-Appropriate Configuration                 |
       |  - Xanadocs: Live System Xanadocs (Format Links, no MD)     |
       |  - Slices: Dynamic System Slices (d.schemas, d.notes)       |
       |  - Gleditor: Plain YAML Configs (user_notes block)          |
       |  - src/ / include/: Fallback structs, typed constexpr       |
       +-------------------------------------------------------------+
```

______________________________________________________________________

## 1. The Three Architect Personas & Roles

### 1.1 The Code Quality Auditor (`code_quality_auditor`)

- **Primary Mission**: Scan the codebase for architectural debt, layer boundary leaks, code
  redundancy, and naked magic numbers.
- **Key Responsibilities**:
  - Detect layer inversions: ensure `include/` and `src/` never include headers from `apps/`.
  - Identify duplicated utility functions, mathematical helpers, or parsing routines across
    `apps/xuzz`, `apps/common/ui`, and the engine in `apps/common/xanadu`.
  - Locate hardcoded literals (e.g. physics spring stiffness, rendering margins, packet timeouts,
    font sizes) that belong in user configuration.
  - Audit system configuration structures for compliance with the supplemental metadata invariant
    (Schema page/cell and User Notes page/cell).

### 1.2 The Architectural Synthesizer (`arch_synthesizer`)

- **Primary Mission**: Formulate clean refactoring plans, design shared abstractions, and execute
  layer promotions from application space to library space.
- **Key Responsibilities**:
  - Design migration plans for moving reusable components developed in `apps/` down to `src/` and
    `include/gleditor/`.
  - Enforce clean interface boundaries: keep domain logic cleanly separated from rendering backends
    (OpenGL, Vulkan).
  - Design system xanadoc and system slice schemas, ensuring header styling uses native format links
    (`bold`, `align-centre`, font scale) rather than markdown syntax.

### 1.3 The Systems Profiler (`systems_profiler`)

- **Primary Mission**: Back every optimization and data structure change with concrete, reproducible
  data.
- **Key Responsibilities**:
  - Verify that structs in hot rendering/physics loops are cache-line aligned (64 bytes) and packed
    efficiently (e.g. `CompactOpNode`).
  - Measure layout latency and memory allocations using dedicated probe tools (e.g.
    `tools/layout-latency-probe.cpp`, `tools/benchmark-kjv-load.py`).
  - Enforce zero-copy principles: zero dynamic allocations on hot interactive render loops and zero
    UI thread blocking for network/DHT lookups.
  - Reject purely speculative optimizations that add cognitive complexity without measurable
    throughput or latency improvements.

______________________________________________________________________

## 2. Configuration & Magic Number Hierarchy

Magic numbers and arbitrary parameters must never be left hardcoded in application logic. Instead,
they must be situated at the appropriate architectural layer according to these strict rules:

### 2.1 Xanadocs in `xuzz`: Live System Xanadocs

In `xuzz`, all user-configurable parameters (e.g., physics spring constants, beam tensions, anchor
bracket colors, notification offsets, keymaps) must be read **live dynamically from an appropriate
system xanadoc** (e.g., `system://keymap`, `system://settings`, `system://layout`, `system://ui`).

#### Invariant: Linked Supplemental Metadata

Every system xanadoc must provide two bidirectionally linked companion pages:

1. **Schema & Purpose Page**: Details the general purpose of the system store, full schema
   definition, key names, data types, physical/typographic units (e.g. pixels, seconds,
   dimensionless ratios), and default fallback values.
1. **User Notes Page**: Dedicated page for the user's personal notes, customization records, and
   rationale.

#### Invariant: Zero Markdown Syntax; Native Format Links

- **NO markdown syntax** (do not use `#`, `##`, `**`, `*`, `_`, or markdown link syntax in xanadoc
  content).
- All page and section titles (such as the `Notes` title on the user notes page and `Schema` on the
  specification page) must be plain text formatted using **Xanadu native format links**
  (`LinkType::Format`):
  - `FormatAttribute::Bold`: renders the header bold.
  - `FormatAttribute::AlignCentre`: centers the header horizontally across the page.
  - Font scale specifier: scales the font to a slightly larger point size.

### 2.2 ZigZag Slices in `xuzz`: System Slices

For slice views, user-configurable parameters (cell dimensions, camera projection angles, step
animation timings, dimension cycle intervals) must be loaded dynamically from a set of **system
zigzag slices**.

#### Invariant: Orthogonal Metadata Dimensions

Every system slice cell must link along standard orthogonal dimensions to its metadata:

1. **`d.schemas` Rank**: Links the configuration cell to a cell specifying its field schema, valid
   value ranges, and physical units.
1. **`d.notes` Rank**: Links the configuration cell to a cell headed with a `Notes` title (no
   markdown syntax) for user observations and custom settings history.

### 2.3 `apps/gleditor` (Plain Editor): YAML Configuration

In `apps/gleditor`, configuration parameters (tab width, line numbers, cursor blink rate, font
fallbacks) must be read from a plain YAML configuration file with well-documented scalar fields and
an explicit `user_notes:` dictionary section.

### 2.4 Core Library (`src/` and `include/gleditor/`): Fallback Defaults & Invariants

- Immutable physical invariants (e.g. cache-line size `64`, HarfBuzz 26.6 fixed-point scaling factor
  `64.0F`, BitTorrent piece size `65536`) must be defined as typed `constexpr` within dedicated
  headers (e.g. `constants.hpp` or domain-specific headers).
- Fallback defaults for configuration structs must be cleanly encapsulated with documented rationale
  comments explaining the default value.
- The library layer must never contain hardcoded application-level assumptions or UI policies.

______________________________________________________________________

## 3. Layer Promotion Protocol

When code in `apps/xuzz`, `apps/common/ui` or `apps/gleditor` matures or proves generally useful, it
must be promoted — xanalogical code to the engine (`apps/common/xanadu/`), everything else to the
core library — following this protocol:

1. **Isolation Audit**: Ensure the candidate component has no implicit dependencies on application
   singletons, application-specific UI, or other apps.
1. **API Abstraction**: Extract a clean, minimal C++23 interface with public headers placed in
   `include/gleditor/<subsystem>/` and implementation in `src/<subsystem>/` (or under
   `apps/common/xanadu/` for engine code).
1. **Data-Backed Verification**: Verify that the extracted component maintains or improves cache
   locality and does not introduce unnecessary memory copies.
1. **Consumer Migration**: Update consumer applications to use the new shared library interface.
1. **Regression Verification**: Run the full test suite (`make -j$(nproc) test`) and visual
   regression verification (`./tools/compare-backends.sh`).

______________________________________________________________________

## 4. Multi-Agent Orchestration & Dialectic

For a project-level code quality or refactoring review, give each of the three personas in §1 its
own subagent when subagents are available, run them concurrently on the same target module or
feature with that persona's mission as the brief, and synthesize their findings into one plan before
implementing changes. Without subagents, work through the three roles in turn.
