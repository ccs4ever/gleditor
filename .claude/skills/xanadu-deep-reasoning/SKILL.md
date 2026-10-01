---
name: xanadu-deep-reasoning
description: >-
  Multi-agent deep reasoning workflow in Xanadulogical architecture.
  Orchestrates a tripartite dialectic between an Ideological Purist (Nelsonian principles),
  a Systems Realist (hardware/graphics/networking constraints), and a Codebase Expert
  (gleditor/xudu/zigzag C++23 implementation details).
---

# Xanadulogical Deep Reasoning: The Tripartite Dialectic

This skill defines the multi-agent reasoning architecture designed to solve complex design,
theoretical, and implementation challenges across the **gleditor**, **xudu** (xanadoc editor), and
**zigzag** (multidimensional visualizer) ecosystems.

```
       +-------------------------------------------------------------+
       |                  Xanadulogical Purist                       |
       |  - Absolute Transclusion (zero copy-paste)                  |
       |  - Universal Docuverse & Infinite Permascrolls              |
       |  - N-Dimensional Orthogonal Zigzag Topology (deep ZZ study) |
       |  - Fine-Grained Character-Level Provenance & Micropayments  |
       |  - Deep Intrinsic Bidirectionality                          |
       |  - Zigzag/Xanadu-Space Convergence (xuzz intertwingularity) |
       +-------------------------------------------------------------+
                                   ▲   │
                     Nelsonian     │   │ Real-World
                     Ideals        │   │ Reality Check
                                   │   ▼
       +-------------------------------------------------------------+
       |                    Systems Realist                          |
       |  - Interactive GPU render latency and throughput                      |
       |  - Cache Lines, Memory Bandwidth & Zero-Copy mmap           |
       |  - BitTorrent Swarm Latency & Non-blocking Timeouts         |
       |  - Lock-Free MVCC & Crash-Resilient Segmented Spools        |
       |  - Human Cognitive Load & Visual Ergonomics                 |
       +-------------------------------------------------------------+
                                   ▲   │
                     Hardware      │   │ Architectural
                     Feasibility   │   │ Integration
                                   │   ▼
       +-------------------------------------------------------------+
       |                   Codebase Expert                           |
       |  - `xudu::Store` (Primedia Spool + 64B CompactOpNode DAG)   |
       |  - `Resolver` & `SwarmContentSource` async piece streaming  |
       |  - Zero-Cairo/Zero-Pango FreeType2+HarfBuzz text engine     |
       |  - `StreamBufferGL` ring uploader & Vulkan 1.3 pipelines    |
       |  - `ZZSpace` / `ZZCell` / `rapidyaml` slice architecture    |
       |  - 3-Way Spring Physics Canvas & AccessKit A11y Tree        |
       +-------------------------------------------------------------+
```

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
  - **Zigzag Deep Research (mandatory)**: Since `xuzz` has merged the xanadoc and Zigzag worlds, the
    Purist must reason from Ted Nelson's original Zigzag/Enfilade sources directly — *Literary
    Machines* 87.1's Zigzag chapters, the Xanadu Green/Gold design notes, `xanadu.com.au`'s ZigZag
    materials, and Nelson's own ZigZag Rhetorical Structure and "Geeks Bearing Gifts" commentary —
    not just this repo's `design/enfilade/*` notes. Ground every claim about ranks, dimensions,
    crums, and enfilades in that primary material before treating `apps/zigzag` or
    `apps/common/xanadu` as the final word on what Zigzag *should* be; the codebase is one
    implementation of the idea, not the idea itself. Treat `d.*` dimensions, `CellRef`, and
    `Manifold`/`ArenaManifold` as candidate mappings of Nelsonian Zigzag concepts to be critiqued
    against the primary sources, not accepted uncritically.
  - **xuzz Intertwingularity**: Because `xuzz` fuses xudu's xanalogical hypertext model with
    zigzag's multidimensional space, the Purist must evaluate any proposal for whether it honors
    *both* halves simultaneously — a xanadoc that is not also addressable as zigzag structure (via
    `OpKind::Structure` / `Manifold`) is an incomplete convergence, and a zigzag space with no
    xanalogical provenance is not truly Xanadu space.

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

- **Philosophy**: Truth grounded in the existing C++23 codebase, history, and test suites.
- **Invariants**:
  - **Spool Structure**: `SegmentedOpsSpool` storing 64-byte `CompactOpNode` and append-only
    `SegmentedPrimediaSpool`.
  - **Text Engine**: `src/text/` using HarfBuzz, FreeType 2, libunibreak, FriBidi, with $O(1)$
    height-budgeted line slicing.
  - **Graphics**: `src/render/` with `StreamBufferGL` dynamic ring buffers and Vulkan SPIR-V
    pipelines.
  - **Build & Quality**: Strict C++23, GNU Make (no CMake), zero compiler warnings,
    `./tools/compare-backends.sh` visual verification, and AccessKit accessibility.

______________________________________________________________________

## 2. The Dialectical Reasoning Protocol

When tackling any new feature, architecture refactor, or Xanadu integration, the orchestration
follows a 4-phase cycle:

### Phase 1: Pure Nelsonian Formulation (Thesis)

- **Agent**: `xanadu_purist`
- **Goal**: Define the purest theoretical model. What would Ted Nelson build if computing power,
  memory, and networking were boundless? How are the dimensions, spools, and transclusion spans
  mathematically structured?
- **Required step for anything touching zigzag, `xuzz`, or cross-cutting hypertext/space
  questions**: before formulating the thesis, do deep research on Project Xanadu's Zigzag from
  primary Nelsonian sources (not just this repo's design notes) — ranks, dimensions, crums,
  enfilades — and explicitly reason about how the xanalogical model (xudu) and the Zigzag
  multidimensional model (zigzag) are meant to be one intertwingled thing in `xuzz`, since the two
  are no longer separable concerns.

### Phase 2: Silicon & Network Stress-Testing (Antithesis)

- **Agent**: `systems_realist`
- **Goal**: Attack the theoretical model with real-world bottlenecks: cache misses, GPU draw call
  limits, network packet loss, P2P churn, memory fragmentation, and cognitive overload. Identify
  where the pure model would stall, deadlock, or exhaust RAM.

### Phase 3: Codebase Grounding & Synthesis (Synthesis)

- **Agent**: `codebase_expert`
- **Goal**: Map the debate directly onto `gleditor`, `apps/xudu`, and `apps/zigzag`. Where do
  existing structures (e.g. `CompactOpNode`, `SwarmContentSource`, `ZZSpace`, `StreamBufferGL`)
  already solve the problem? What precise C++23 structs, APIs, or database schemas bridge the pure
  ideal with practical hardware constraints?

### Phase 4: Unified Architectural Convergence

- **Leader**: Orchestrator (Antigravity)
- **Goal**: Formulate the final engineering specification that maximizes Xanadulogical fidelity
  while maintaining responsive, empirically verified performance and rock-solid system stability.

______________________________________________________________________

## 3. How to Invoke the Tripartite Agents

The agents can be invoked concurrently using `invoke_subagent`:

```json
{
  "Subagents": [
    {
      "TypeName": "xanadu_purist",
      "Role": "Xanadulogical Ideological Purist",
      "Prompt": "<Design challenge or architectural question>",
      "Model": "inherit"
    },
    {
      "TypeName": "systems_realist",
      "Role": "Systems & Hardware Realist",
      "Prompt": "<Design challenge or architectural question>",
      "Model": "inherit"
    },
    {
      "TypeName": "codebase_expert",
      "Role": "Gleditor Codebase Expert",
      "Prompt": "<Design challenge or architectural question>",
      "Model": "inherit"
    }
  ]
}
```

The orchestrator sends cross-critiques between agents using `send_message` until consensus and
optimal synthesis are reached.
