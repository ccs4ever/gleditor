---
name: system-architect
description: >-
  Project-level refactoring, layer promotion, DRY consolidation, data-backed optimization, and
  system://* vs TSV config governance. Use when auditing code quality, eliminating magic numbers,
  promoting code across layers, or profiling performance.
---

# System Architect Agentic Workflow: Project-Level Code Quality

This skill defines the multi-agent architecture and operational protocols for maintaining modular
architectural layering, empirical performance optimization, and clean layer-appropriate
configuration across **`gleditor`** (core library and plain editor) and **`xuzz`**, the one
application for xanadocs and ZigZag slices. `xudu` and `zigzag` are retired as applications and
there is no `apps/xudu/` or `apps/zigzag/`. The library stays generic (`apps/gleditor` uses it and
has no Xanadu reference); xanalogical code goes in `apps/common/` (`apps/common/xanadu/` without a
graphics device, `apps/common/ui/` with one), and `apps/xuzz/` holds only what is unique to that
program. Library tests live in `tests/lib/` and are not repeated in `xuzz_test`.

______________________________________________________________________

## 1. The Three Architect Personas & Roles

### 1.1 The Code Quality Auditor (`code_quality_auditor`)

- **Primary Mission**: Scan the codebase for architectural debt, layer boundary leaks, code
  redundancy, and naked magic numbers.
- **Key Responsibilities**:
  - Detect layer inversions: ensure `include/` and `src/` never include headers from `apps/`.
  - Identify duplicated utility functions, mathematical helpers, or parsing routines across
    `apps/xuzz`, `apps/common/ui`, and the engine in `apps/common/xanadu`.
  - Locate hardcoded literals (physics spring stiffness, rendering margins, packet timeouts, font
    sizes) that belong in user configuration.
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
  - Design system xanadoc and system slice schemas with native Xanadu format links (`Bold`,
    `AlignCentre`, font scale) rather than markdown syntax.

### 1.3 The Systems Profiler (`systems_profiler`)

- **Primary Mission**: Back every optimization and data structure change with concrete, reproducible
  data.
- **Key Responsibilities**:
  - Verify that structs in hot rendering/physics loops are cache-line aligned (64 bytes) and packed
    efficiently (e.g. `CompactOpNode`).
  - Measure layout latency and memory allocations using dedicated probe tools
    (`tools/layout-latency-probe.cpp`, `tools/benchmark-kjv-load.py`, `make ui-text-baseline`).
  - Enforce zero-copy principles: zero dynamic allocations on hot interactive render loops and zero
    UI thread blocking for network/DHT lookups.
  - Reject purely speculative optimizations that add cognitive complexity without measurable
    throughput or latency improvements.

______________________________________________________________________

## 2. Configuration & Magic Number Audit Summary

Magic numbers and arbitrary parameters must never be left hardcoded in application logic:

- **Xanadocs & ZigZag slices in `xuzz`:** Read user-configurable values **live dynamically** from
  `system://*` xanadocs or sovereign `xanadu::Store`-backed system slices (zero markdown syntax;
  Schema & `Notes` pages styled via `LinkType::Format`, or `d.schemas` and `d.notes` ranks). See
  [`xanadu-engine`](../xanadu-engine/SKILL.md) for the canonical `system://*` schema, `d.*`
  dimension, and `LinkType::Format` specification.
- **`apps/gleditor` (Plain Editor):** Load parameters from a **plain TSV configuration file**
  (`apps/gleditor/editor_config.cpp`) with a `user_notes.notes` entry.
- **Core Library (`src/` and `include/gleditor/`):** Define immutable physical invariants (e.g.,
  cache-line size `64`, HarfBuzz 26.6 scaling `64.0F`, BitTorrent piece size `65536`) as typed
  `constexpr` in dedicated headers with rationale comments; document rationale on struct fallback
  defaults and never hardcode app policies.

______________________________________________________________________

## 3. Layer Promotion Protocol

When code in `apps/xuzz`, `apps/common/ui`, or `apps/gleditor` matures or proves generally useful,
promote it — xanalogical code to `apps/common/xanadu/`, everything else to `libgleditor`:

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
