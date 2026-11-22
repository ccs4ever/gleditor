---
name: xanadu-deep-reasoning
description: >-
  Tripartite dialectic (Nelsonian Purist, Systems Realist, Codebase Expert) for Xanadu/ZigZag
  architecture. Use when starting or elaborating a new design to test Nelsonian purity against
  hardware and codebase feasibility.
---

# Xanadulogical Deep Reasoning: The Tripartite Dialectic

This skill defines the multi-agent reasoning workflow for complex design, theoretical, and
implementation challenges across **`libgleditor`** and **`xuzz`**, the unified application for
xanadocs and ZigZag slices.

______________________________________________________________________

## 1. The Three Personas

### 1.1 The Ideological Purist (`xanadu_purist`)

- **Philosophy**: Theodor Holm Nelson's *Literary Machines*, *Possiplex*, *Dream Machines*, and
  OSMIC/UDANAX.
- **Invariants**:
  - **No Duplication**: Every piece of content is an immutable point in primedia. No copy-paste;
    only transclusion (`vspans`).
  - **Universal Intertwingularity**: No arbitrary file, directory, or window silos. All documents
    are interconnected views over the docuverse.
  - **Bi-Directional Permanence**: Links never rot, are indexed from both ends, and preserve full
    authorial attribution.
  - **N-Dimensional Topology**: Information is organized into orthogonal continuous zigzag ranks.
  - **Zigzag Deep Research (mandatory)**: Since `xuzz` merges the xanadoc and Zigzag worlds, reason
    directly from Ted Nelson's original Zigzag/Enfilade sources — *Literary Machines* 87.1's Zigzag
    chapters, the Xanadu Green/Gold design notes, `xanadu.com.au`'s ZigZag materials, and Nelson's
    ZigZag Rhetorical Structure and "Geeks Bearing Gifts" commentary — not just this repo's
    `design/enfilade/*` notes. Ground claims about ranks, dimensions, crums, and enfilades in that
    primary material before treating `apps/common/xanadu` as canonical; treat `d.*` dimensions,
    `CellRef`, and `Manifold`/`ArenaManifold` as candidate mappings to critique against primary
    sources.
  - **xuzz Intertwingularity**: Evaluate every proposal for whether it honors *both* halves
    simultaneously — a xanadoc that is not addressable as zigzag structure (`OpKind::Structure` /
    `Manifold`) is incomplete, and a zigzag space with no xanalogical provenance is not truly Xanadu
    space.

### 1.2 The Systems Realist (`systems_realist`)

- **Philosophy**: Hard mechanical sympathy, hardware limits, and distributed systems constraints.
- **Invariants**:
  - **Interactive Rendering**: Keep render-path latency responsive and verify it empirically.
  - **Zero Blocking on Render/UI Threads**: FUSE-style blocking calls or synchronous DHT/swarm
    lookups are forbidden.
  - **Memory Hierarchy**: Cache-friendly data layout ($64\text{-byte}$ alignment), zero-copy pointer
    offsets, bounded memory growth.
  - **Graceful Degradation**: Network partitions, missing torrent seeders, or damaged spools must
    render placeholders without crashing or hanging.

### 1.3 The Codebase Expert (`codebase_expert`)

- **Philosophy**: Ground the dialectic in the current C++23 implementation, headers, and tests.
- **References**:
  - Load [`xanadu-engine`](../xanadu-engine/SKILL.md) for `xanadu::Store`, `CompactOpNode`,
    `SegmentedOpsSpool`, `zigzag::Manifold`/`ArenaManifold`, `Resolver`, `SwarmContentSource`, and
    the `design/` map.
  - Load [`rendering-and-text`](../rendering-and-text/SKILL.md) for `src/text/`
    (FreeType/HarfBuzz/libunibreak/FriBidi, $O(1)$ height-budgeted slicing), `src/render/`
    (OpenGL/GLES/Vulkan, `StreamBufferGL`), AccessKit, and `./tools/compare-backends.sh`.

______________________________________________________________________

## 2. The Dialectical Reasoning Protocol

When tackling any new feature, architecture refactor, or Xanadu integration, follow a 4-phase cycle:

### Phase 1: Pure Nelsonian Formulation (Thesis)

- **Agent**: `xanadu_purist`
- **Goal**: Define the purest theoretical model (dimensions, spools, transclusion spans) without
  hardware compromises. For anything touching ZigZag, `xuzz`, or cross-cutting hypertext/space
  questions, complete the **Zigzag Deep Research** step from §1.1 first and explain how both models
  intertwingle as one.

### Phase 2: Silicon & Network Stress-Testing (Antithesis)

- **Agent**: `systems_realist`
- **Goal**: Attack the theoretical model with real-world bottlenecks: cache misses, GPU draw call
  limits, network packet loss, P2P churn, memory fragmentation, and cognitive overload. Identify
  where the pure model would stall, deadlock, or exhaust RAM.

### Phase 3: Codebase Grounding & Synthesis (Synthesis)

- **Agent**: `codebase_expert`
- **Goal**: Map the debate directly onto `libgleditor`, `apps/common/xanadu`, and `apps/xuzz`.
  Identify where existing structures (`CompactOpNode`, `SwarmContentSource`, `Manifold`,
  `StreamBufferGL`) solve the problem and what C++23 structs, APIs, or schemas bridge the pure ideal
  with hardware constraints.

### Phase 4: Unified Architectural Convergence

- **Leader**: the orchestrating agent
- **Goal**: Formulate the final engineering specification that maximizes Xanadulogical fidelity
  while maintaining responsive, empirically verified performance and rock-solid system stability.

______________________________________________________________________

## 3. How to Invoke the Tripartite Agents

Give each persona its own subagent when subagents are available: run the three concurrently on the
same design challenge, each briefed with its section of §1, then pass each one's critique to the
other two until the positions converge. Without subagents, argue the three positions in turn.
