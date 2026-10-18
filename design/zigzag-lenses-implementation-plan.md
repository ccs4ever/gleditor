# Plan 9 Zigzag Lenses: Implementation Plan

**Document Version:** 1.0  
**Status:** Plan, unbuilt.  
**Branch:** `feat/zigzag-lenses`  
**Date:** October 18, 2026  
**Foundational Specifications:**
- [`design/zigzag-lenses-plan9-philosophy.md`](zigzag-lenses-plan9-philosophy.md) (Core Architecture)
- [`design/zigzag-lenses-tripartite-critique.md`](zigzag-lenses-tripartite-critique.md) (Multi-Expert Critique & Rectification)
- [`design/zigzag-lenses-enfilade-synthesis.md`](zigzag-lenses-enfilade-synthesis.md) (Enfilade Acceleration Specification)
- [`design/view-system.md`](view-system.md) (Pluggable View System)
- [`design/vortex-distribution.md`](vortex-distribution.md) (Spool-Mesh Distributed Runtime)
- [`design/anything-to-zigzag-lens.md`](anything-to-zigzag-lens.md) (`ProjectionLens` & `ProjectionSession`)

---

## 1. How to Use This Plan

This plan organizes the implementation of the **Dimension-Cell Operating Environment (Zigzag Lenses
under the Plan 9 Philosophy)** into concrete, verifiable packages. It incorporates all rulings from
the seven expert audits (Plan 9, Technical Writer, Distributed Systems, System Architect, Codebase,
DevOps, and Xanadu Purist) and the Project Xanadu Enfilade Expert.

- **§2** records the corrections and structural realignments from the audits that govern the work.
- **§3** specifies the six exploratory spikes that must pass before packages commence.
- **§4** defines the work across five parallel tracks, structured into six delivery milestones (M0–M5).
- **§5** details the quality, invariant verification, and performance gates each commit must pass.
- **§6** is the risk register with mitigation triggers.

### Package Discipline
Every package is self-contained, keeps `make test` green, and includes its own unit tests.
Package sizing:
- **S**: Under 300 lines of implementation + tests (1 commit).
- **M**: 300 to 800 lines (1–2 commits).
- **L**: 800 to 1,500 lines (2–3 commits).
- **XL**: Over 1,500 lines (multi-commit track).

---

## 2. Structural Realignments & Invariants

The implementation plan enforces the following non-negotiable architectural realignments:

| # | Domain | Audit Finding | Implementation Mandate |
| :--- | :--- | :--- | :--- |
| **A1** | **Repository Boundary** | `ILens` with `draw()` and `StreamBufferGL` in `apps/common/xanadu/` breaks headless compilation. | `apps/common/xanadu/` is **100% headless**. Tier 1 source lenses implement `zigzag::ProjectionLens`. Tier 3 presentation views implement `xanadu::view::SliceView` (pure layout in `apps/common/xanadu/view/slice/`). GPU drawing, picking, and AccessKit reside in `apps/common/ui/view/`. |
| **A2** | **Topological Invariant** | Process trees using $+d.\text{child}/-d.\text{parent}$ violate $\text{deg}^\pm \le 1$ when a parent has multiple children. | Canonical Nelsonian family tree: $d.\text{child}$ links to the **firstborn child**; children link to one another along orthogonal **`d.sibling`**. Trees are represented by two orthogonal dimensions. |
| **A3** | **Dimension Purity** | Slashes (`d.net -> d.tcp`) and struct fields (`d.peer_addr`) bastardize dimensions. | Dimensions are universal coordinate axes of variation. Slashes are banned. Endpoint values are carried in OSMIC R6 typed cell slots. Relational dimensions standardized: `d.endpoint`, `d.attribute`, `d.param`. |
| **A4** | **Clone Invariant** | Using `d.clone` for socket/process allocation violates the Master Headcell definition. | `d.clone` is strictly reserved for identical information atoms sharing a negward Master Headcell. Dynamic allocations must link along `d.instance` or `d.session`. |
| **A5** | **9Z2026 Wire Protocol** | The proposed 7-message 9Z lacks session lifecycle, resource cleanup, in-flight cancellation, and metadata queries. | 9Z is expanded to **12 message pairs** mirroring 9P2000: `Tversion`, `Tauth`, `Tattach`, `Tclunk`, `Tflush`, `Zstat`, `Zhop`, `Zread`, `Zdims`, `Zlink`, `Zmake`, `Zwatch`, and universal `Rerror(errstr)`. |
| **A6** | **Control Concurrency** | Shared `d.ctl` ranks create multi-writer collisions violating $\text{deg}^\pm \le 1$. | **Command Transaction Pattern**: Clients mint private Transaction cells linked to target with a $d.\text{reply}$ Promise cell. Local synthetic lenses evaluate commands synchronously during linking. |
| **A7** | **CRDT-ZZ Convergence** | Concurrent remote links to $(C, +d)$ break $\text{deg}^\pm \le 1$ under split-brain. | **Strategy A (Sequential Ranks)**: Deterministic Rank Sequence Splice (RGA total order over $\langle \text{parentMicroversion}, \text{GlobalOpRef}, \text{authorKey} \rangle$).<br>**Strategy B (Functional Attributes)**: Orthogonal Collision Dimension Promotion ($d.\text{conflict}(d, \text{hash}(V_0))$). |
| **A8** | **Enfilade Replay Rule** | Raw CSR rank walks are $O(N)$, bottlenecking scrollbars, layout, filtering, and telemetry. | Under **Ruling R8**, enfilades are **ephemeral replay products, never cells**. They mint zero `CompactOpNode`s and leave `CellSlot` at exactly 32 bytes (R12/U3). |
| **A9** | **Memory Lifecycles** | Long-running synthetic monitors endlessly grow append-only `ArenaManifold` slots and links. | Windowed materialization + $O(1)$ **Epoch Tosses** (`release(emptyMark)`), using `ExternalKey` for cross-epoch identity. |
| **A10** | **Cross-Boundary Linking** | Persistent `Store` on NVMe must never hold ephemeral `CellRef`s (`ephemeralBit = 0x80000000U`). | Document links to synthetic entities must target a **Persistent Proxy Cell** (`ValueKind::ForeignOpHandle` / URI span), dynamically bound to the live ephemeral `CellRef` by `ProjectionSession`. |

---

## 3. Exploratory Spikes

The spikes resolve critical performance and algorithmic questions before package implementation.
All spikes run headless, require $\le 1$ day each, and gate their respective tracks.

| # | Spike Question | Methodology | Pass Criteria | Gates |
| :--- | :--- | :--- | :--- | :--- |
| **S1** | **Poomfilade Rank Indexing Overhead** | Mint 500,000 cells in `ArenaManifold`. Build a `Rankfilade` ($B=8$). Measure $i \to C$ descent and $C \to i$ ascent. | Descent $\le 35\text{ ns}$; Ascent $\le 45\text{ ns}$; Zero heap allocation during traversal. | P1, P3 |
| **S2** | **9Z2026 Wire Serialization Round-Trip** | Benchmark 9Z frame encoder/decoder over `LoopbackChannel` with 12 message pairs. | $\le 1.2\ \mu\text{s}$ per message pair; zero heap allocation on stack frames; `Rerror` strings preserved. | P2 |
| **S3** | **CRDT-ZZ Conflict Promotion** | Generate concurrent synthetic `Zlink` operations on $(C, +d)$ from 4 virtual nodes; merge locally. | Winning link stays on $d$; conflicting edges promoted to $d.\text{conflict}$; $\text{deg}^\pm \le 1$ strictly verified. | P3 |
| **S4** | **Layoutfilade Viewport Stabbing** | Index 100,000 flow lines with non-uniform heights and media boxes. Query `visibleRange(topY, bottomY)`. | Viewport stabbing $\le 50\text{ ns}$; localized line-edit reflow $\le 150\text{ ns}$; zero downstream invalidation. | V2, V3 |
| **S5** | **ArenaManifold Epoch Toss Cost** | Populate 100,000 ephemeral cells in `ArenaManifold`. Execute `release(emptyMark)` (toss). | Flat execution time $\le 10\ \mu\text{s}$; zero name lookups; slab pointers reset cleanly. | T1, T3 |
| **S6** | **Treefilade DDSketch Fleet Rollup** | Simulate 50,000 host telemetry streams. Aggregate `StatWid` sketches across a 4-level Treefilade. | Root p99 query $\le 1\text{ ms}$; tree update latency $\le 15\text{ ms}$; memory $\le 64\text{ MB}$ total. | D3 |

---

## 4. The Work: Tracks and Packages

```
TRACK 1: PROTOCOL & ENFILADE CORE (P1 - P3)
  └── Gates: S1, S2, S3

TRACK 2: SYNTHETIC OS LENSES (T1 - T4)
  └── Gates: S5

TRACK 3: TRANSDUCTIVE ENFILADE PIPELINES (R1 - R4)
  └── Gates: S1, S4

TRACK 4: PRESENTATION LENSES & UI (V1 - V4)
  └── Gates: S4

TRACK 5: DISTRIBUTED SPOOL-MESH & FLEET TELEMETRY (D1 - D4)
  └── Gates: S2, S3, S6
```

---

### Track 1: Protocol & Enfilade Core (`apps/common/xanadu/enfilade/`, `zigzag/`)

#### Package P1: `Rankfilade` & Poomfilade Implementation
- **Size**: M (650 lines) | **Gates**: S1
- **Files**:
  - `apps/common/xanadu/enfilade/rankfilade.hpp`
  - `apps/common/xanadu/enfilade/rankfilade.cpp`
  - `tests/xudu/rankfilade_test.cpp`
- **Scope**:
  - Implements `RankDsp` ($\Delta i$) and `RankWid` ($\langle N \rangle$) monoids adhering to `EnfiladeAction`.
  - Cache-conscious $B$-enfilade ($B=8$) storing relative offsets.
  - Implements `cellAtOrdinal(std::size_t i)` ($O(\log N)$ descent).
  - Implements `ordinalOfCell(CellRef c)` ($O(\log N)$ ascent via auxiliary `LeafHandle` table, preserving the 32-byte `CellSlot` per R12/U3).
- **Tests**: 100k cell rank random-access; insertions at head, tail, and interior renumbering zero downstream crums; scrollbar thumb ratio calculation.

#### Package P2: 9Z2026 Wire Protocol Codec
- **Size**: L (1,100 lines) | **Gates**: S2
- **Files**:
  - `apps/common/xanadu/zigzag/zz9p.hpp`
  - `apps/common/xanadu/zigzag/zz9p.cpp`
  - `tests/xudu/zz9p_test.cpp`
- **Scope**:
  - Full 12-message pair wire codec: `Tversion`/`Rversion`, `Tauth`/`Rauth`, `Tattach`/`Rattach`, `Tclunk`/`Rclunk`, `Tflush`/`Rflush`, `Zstat`/`Rstat`, `Zhop`/`Rhop`, `Zread`/`Rread`, `Zdims`/`Rdims`, `Zlink`/`Rlink`, `Zmake`/`Rmake`, `Zwatch`/`Rwatch`, and universal `Rerror(errstr)`.
  - Bencode serialization for transport over `MeshChannel` and raw sockets.
  - Frame bounds enforcement (`kMaxPayloadBytes = 64 KB`).
- **Tests**: Round-trip codec tests for all 12 message pairs; malformed payload refusal; `errstr` propagation; in-flight `Tflush` cancellation.

#### Package P3: CRDT-ZZ Resolution Engine
- **Size**: M (750 lines) | **Gates**: S3
- **Files**:
  - `apps/common/xanadu/zigzag/crdt_zz.hpp`
  - `apps/common/xanadu/zigzag/crdt_zz.cpp`
  - `tests/xudu/crdt_zz_test.cpp`
- **Scope**:
  - Strategy A: Deterministic Rank Sequence Splice (RGA total order over $\langle \text{parentMicroversion}, \text{GlobalOpRef}, \text{authorKey} \rangle$).
  - Strategy B: Orthogonal Collision Dimension Promotion ($d.\text{conflict}(d, \text{hash}(V_0))$).
  - Enforces the 2-rank manifold invariant ($\text{deg}^\pm(c, d) \le 1$) across concurrent multi-peer write-sets.
- **Tests**: 3-node concurrent link race on same cell and dimension; sequential convergence; conflict promotion verification.

---

### Track 2: Synthetic OS Lenses (Tier 1, `apps/common/xanadu/zigzag/`)

#### Package T1: `ProcLens` (Processes as Manifold Cells)
- **Size**: L (1,200 lines) | **Gates**: S5
- **Files**:
  - `apps/common/xanadu/zigzag/proc_lens.hpp`
  - `apps/common/xanadu/zigzag/proc_lens.cpp`
  - `tests/xudu/proc_lens_test.cpp`
- **Scope**:
  - Implements `zigzag::ProjectionLens` over Linux `/proc` and `pidfd`.
  - Process cells linked via $d.\text{child}$ (firstborn child) and $d.\text{sibling}$ (birth order), strictly preserving $\text{deg}^\pm \le 1$.
  - Thread ranks linked along $d.\text{threads}$.
  - Scalar process metrics (CPU%, memory RSS, state) stored in-place in 32-byte `CellSlot::valueBits`.
  - Driven by `ProjectionSession` into `ArenaManifold`.
  - Windowed materialization with $O(1)$ epoch tosses.
- **Tests**: Hierarchy verification of simulated process tree; sibling traversal; PID exit tombstoning; zero memory leakage across 1,000 epoch tosses.

#### Package T2: Command Transaction Mailbox & Promise Cells
- **Size**: M (600 lines) | **Gates**: P2, T1
- **Files**:
  - `apps/common/xanadu/zigzag/command_transaction.hpp`
  - `apps/common/xanadu/zigzag/command_transaction.cpp`
  - `tests/xudu/command_transaction_test.cpp`
- **Scope**:
  - Replaces shared multi-writer $d.\text{ctl}$ ranks with the Command Transaction pattern.
  - Client mints private Transaction cell linked to target ($+d.\text{target}$), action verb ($+d.\text{action}$), and a Promise cell on $+d.\text{reply}$.
  - Local synthetic lenses evaluate commands synchronously during link operations, populating the Promise cell with `OK` or `Error(errstr)`.
- **Tests**: Concurrent command submission from 10 client threads to one process cell; immediate synchronous failure return on invalid PID; promise resolution.

#### Package T3: `NetLens` (Network Sockets & Streams)
- **Size**: M (700 lines) | **Gates**: S5, T1
- **Files**:
  - `apps/common/xanadu/zigzag/net_lens.hpp`
  - `apps/common/xanadu/zigzag/net_lens.cpp`
  - `tests/xudu/net_lens_test.cpp`
- **Scope**:
  - Network connection prototypes allocated via $d.\text{instance}$ (sanctifying $d.\text{clone}$).
  - Connection cells link to typed address/port cells along $d.\text{endpoint}$, and state along $d.\text{state}$.
  - Bulk packet streams bypass `ArenaManifold` cells and flow through circular ring buffers; cells carry stream handles only.
- **Tests**: Mock TCP connection state machine progression; $d.\text{instance}$ allocation verification; ring buffer read/write integration.

#### Package T4: `VfsLens` (Directory Trees & Inotify)
- **Size**: M (550 lines) | **Gates**: T1
- **Files**:
  - `apps/common/xanadu/zigzag/filesystem_lens.hpp` (extend existing)
  - `apps/common/xanadu/zigzag/filesystem_lens.cpp`
  - `tests/xudu/projection_lens_test.cpp`
- **Scope**:
  - Re-expresses `FilesystemLens` to enforce $d.\text{child}$ (firstborn) and $d.\text{sibling}$ for directory hierarchies.
  - Integrates `inotify` for reactive invalidation via `ProjectionObserver`.
- **Tests**: Directory tree traversal; live file creation inotify event triggering incremental cell minting.

---

### Track 3: Transductive Enfilade Pipelines (Tier 2, `apps/common/xanadu/view/`)

#### Package R1: `TransductiveView` & Materialized Derived Ranks
- **Size**: L (950 lines) | **Gates**: P1
- **Files**:
  - `apps/common/xanadu/view/transductive_view.hpp`
  - `apps/common/xanadu/view/transductive_view.cpp`
  - `tests/xudu/transductive_view_test.cpp`
- **Scope**:
  - Engine-level derivation of ephemeral dimensions ($d.\text{filtered}$, $d.\text{sorted}$) in `ViewManifold`'s `derived_` arena.
  - Operates purely during `prepare()`; rendering walks concrete CSR runs in $4.96\text{ ns}$ with zero dynamic virtual predicate overhead.
  - Read-only union views evaluated via cycle-bounded range iterators.
- **Tests**: 10,000-cell rank filter; CSR link generation in derived arena; cycle detection in malformed union rank.

#### Package R2: `Arrayfilade` Predicate Pushdown Planner
- **Size**: M (700 lines) | **Gates**: R1
- **Files**:
  - `apps/common/xanadu/enfilade/arrayfilade.hpp` (integrate with lenses)
  - `apps/common/xanadu/enfilade/arrayfilade.cpp`
  - `tests/xudu/arrayfilade_lens_test.cpp`
- **Scope**:
  - Bounding hull $[\mathbf{c}_{\min}, \mathbf{c}_{\max}]$, scalar range $[s_{\min}, s_{\max}]$, and 64-bit Bloom filter pushdown.
  - $O(1)$ subtree pruning for range filters ($s \in [a, b]$) and exact-match string filters.
  - Zero-copy slicing `slice([minCoord, maxCoord])` yielding an ephemeral sub-enfilade.
- **Tests**: Slicing benchmark; pruning efficiency verification (skipping $>95\%$ of crums on 1% selectivity query).

#### Package R3: `Spanfilade` Optical Transclusion Stabbing
- **Size**: M (600 lines) | **Gates**: S4
- **Files**:
  - `apps/common/xanadu/enfilade/spanfilade.hpp` (integrate with view system)
  - `apps/common/xanadu/enfilade/spanfilade.cpp`
  - `tests/xuzz/spanfilade_beam_test.cpp`
- **Scope**:
  - Viewport interval stabbing: Given the active lens viewport span $[S, S + L)$, query `Spanfilade` to return all intersecting transclusion and link endpoints in $O(\log N + K)$.
  - Feeds `BridgeCoordinator` in $< 20\ \mu\text{s}$ to generate cubic Bézier optical beams.
- **Tests**: Stabbing across 10 open documents with 500 transclusions; verification of complete link endpoint recovery.

---

### Track 4: Presentation Lenses (Tier 3, `apps/common/xanadu/view/slice/` & `apps/common/ui/view/`)

#### Package V1: `LatticeSliceView` (Pure Spatial Layout)
- **Size**: L (1,100 lines) | **Gates**: P1, R1
- **Files**:
  - `apps/common/xanadu/view/slice/lattice_view.hpp`
  - `apps/common/xanadu/view/slice/lattice_view.cpp`
  - `tests/xuzz/lattice_view_test.cpp`
- **Scope**:
  - Extracted from legacy `ZigzagVisualizer`: pure spatial layout function implementing `xanadu::view::SliceView`.
  - Places cells as 3D cards in world space along $(X, Y, Z)$ bound dimensions.
  - Zero graphics or SDL dependencies; outputs `LayoutSink`.
- **Tests**: Deterministic coordinate calculation for $3 \times 3 \times 3$ cell cluster; camera tracking math; bounds verification.

#### Package V2: `XanadocSliceView` with `Layoutfilade` Virtualization
- **Size**: L (1,250 lines) | **Gates**: S4, V1
- **Files**:
  - `apps/common/xanadu/view/slice/xanadoc_view.hpp`
  - `apps/common/xanadu/view/slice/xanadoc_view.cpp`
  - `tests/xuzz/xanadoc_view_test.cpp`
- **Scope**:
  - Unrolls $d.\text{contents}$ into typographic prose using `Layoutfilade` for $O(\log N)$ viewport stabbing.
  - Zero off-screen text shaping through HarfBuzz/FreeType.
  - Localized reflow on cell edits without touching downstream crums.
- **Tests**: Virtualized scroll test over 50,000-line xanadoc; frame time $\le 1\text{ ms}$; reflow latency verification.

#### Package V3: `AcmeSliceView` (Spatial Tiling & Fluid Tags)
- **Size**: L (1,300 lines) | **Gates**: T2, V1
- **Files**:
  - `apps/common/xanadu/view/slice/acme_view.hpp`
  - `apps/common/xanadu/view/slice/acme_view.cpp`
  - `tests/xuzz/acme_view_test.cpp`
- **Scope**:
  - Spatial pane tiling along $d.\text{layout\_x}$ and $d.\text{layout\_y}$.
  - Tag rank carries fluid, editable text buffer cells (not rigid button toolbars).
  - Exposes $d.\text{event}$ per pane for external script interception (replicating `/dev/acme/N/event`).
  - B1 (select/sweep), B2 (execute Vortex verb or OS action), B3 (structural link follow or literal text search).
- **Tests**: Column split and pane insertion; B2 chord execution dispatch; B3 structural link navigation vs text search fallback.

#### Package V4: UI View Presenters & Hardware Bridging
- **Size**: L (1,400 lines) | **Gates**: V1, V2, V3
- **Files**:
  - `apps/common/ui/view/slice/slice_view_presenter.hpp`
  - `apps/common/ui/view/slice/slice_view_presenter.cpp`
  - `apps/common/ui/view/slice/acme_presenter.hpp`
  - `apps/common/ui/view/slice/acme_presenter.cpp`
- **Scope**:
  - Bridges `LayoutSink` to `libgleditor` rendering facilities (`Renderer`, `StreamBufferGL`, Vulkan pipelines).
  - Implements `gleditor::FrameContributor`, `PickObserver`, and `AccessKit` source.
- **Tests**: Visual comparison against baseline backends (`tools/compare-backends.sh`); picking hit-test accuracy; AccessKit accessibility tree audit.

---

### Track 5: Distributed Spool-Mesh & Fleet Telemetry (`apps/common/xanadu/vortex/`)

#### Package D1: `MeshLensChannel` & Foreign Proxy Cells
- **Size**: L (1,200 lines) | **Gates**: P2, P3
- **Files**:
  - `apps/common/xanadu/vortex/mesh_lens_channel.hpp`
  - `apps/common/xanadu/vortex/mesh_lens_channel.cpp`
  - `tests/xudu/mesh_lens_channel_test.cpp`
- **Scope**:
  - Connects `ProjectionSession` to `SpoolMesh` over the BEP 10 actor wire (`xudu_vortex_actor`).
  - Implements non-blocking 4-state proxy cell state machine: `Unmaterialized` $\to$ `Resolving` $\to$ `Active` $\to$ `Disconnected`/`Tombstone`.
  - Directional **Frontier Expansion** speculative prefetching along dimensional momentum vectors.
- **Tests**: Simulated 2-peer network hop; 4-state transition on timeout; UI render loop never blocks when proxy is `Disconnected`.

#### Package D2: `Treefilade` Fleet Telemetry Aggregator
- **Size**: L (1,100 lines) | **Gates**: S6, D1
- **Files**:
  - `apps/common/xanadu/enfilade/treefilade.hpp`
  - `apps/common/xanadu/enfilade/treefilade.cpp`
  - `tests/xudu/treefilade_test.cpp`
- **Scope**:
  - Statistical summary monoid `StatWid`: Count $N$, sum $\Sigma$, sum of squares $\Sigma x^2$, mergeable DDSketch quantile sketch, HyperLogLog mask, and anomaly bitflags.
  - Aggregation along $d.\text{node} \to d.\text{zone} \to d.\text{region} \to d.\text{fleet}$.
  - Root crum answers fleet-wide p99 queries in $O(1)$.
  - Anomaly bitflags trigger reactive 9Z `Zwatch` pulses along $d.\text{alerting}$.
- **Tests**: 50,000-node simulated metric aggregation; p99 latency accuracy within $0.5\%$; reactive alert dispatch.

#### Package D3: `ReconcileLens` (Declarative Hypertime Rollback)
- **Size**: M (750 lines) | **Gates**: T1, D1
- **Files**:
  - `apps/common/xanadu/zigzag/reconcile_lens.hpp`
  - `apps/common/xanadu/zigzag/reconcile_lens.cpp`
  - `tests/xudu/reconcile_lens_test.cpp`
- **Scope**:
  - Declarative reconciliation state machine:
    $$\text{Diff}(d.\text{actual\_state}, d.\text{desired\_config}) \longrightarrow \text{ActionPlan}$$
  - Unwinds physical OS side-effects (terminating processes, unbinding ports) when $d.\text{active\_config}$ is retargeted backward in hypertime.
  - Phased rollouts sequenced along $d.\text{phase}$ with cryptographic canary barrier cells.
- **Tests**: Hypertime rollback of simulated multi-process service configuration; verification of zero leaked OS processes or open ports.

#### Package D4: Dimensional Capability Tokens (`DimCap`) & NVRAM Crash Spool
- **Size**: M (700 lines) | **Gates**: P2, D1
- **Files**:
  - `apps/common/xanadu/zigzag/dim_cap.hpp`
  - `apps/common/xanadu/zigzag/dim_cap.cpp`
  - `tests/xudu/dim_cap_test.cpp`
- **Scope**:
  - Fine-grained object-capability tokens (Macaroons) gating individual dimensions ($d.\text{threads}$ allowed, $d.\text{ctl}$ forbidden).
  - Mandatory audit context metadata on $d.\text{ctl}$ action cells ($d.\text{actor}$, $d.\text{ticket}$, $d.\text{signature}$).
  - Persistent circular crash spooling (`KdumpOpsSpool`) in simulated NVRAM sealed into an immutable hypertime branch upon reboot.
- **Tests**: Permission denial on unauthorized $d.\text{ctl}$ link attempt; non-repudiable audit log verification; post-panic hypertime trace recovery.

---

## 5. Milestone Delivery Schedule

```
+───────────────────────────────────────────────────────────────────────────+
|                           MILESTONE ROADMAP                               |
+───────────────────────────────────────────────────────────────────────────+

  M0: Enfilade & 9Z Wire Foundation (Spikes S1-S6, P1, P2, P3)
  └── Deliverable: Rankfilade O(log N) indexing, 9Z2026 codec, CRDT-ZZ tests.

  M1: Headless Synthetic OS Engine (T1, T2, T4)
  └── Deliverable: ProcLens running in headless xudu_test, Transaction Mailbox.

  M2: Transductive Enfilade Derivations (R1, R2, R3)
  └── Deliverable: FilterLens with Arrayfilade predicate pushdown, Spanfilade beams.

  M3: Virtualized Presentation Lenses (V1, V2, V3, V4)
  └── Deliverable: LatticeView, XanadocView, AcmeView rendering at 120 FPS.

  M4: Distributed Spool-Mesh Lenses (D1, D3, D4)
  └── Deliverable: 2-node peer mesh mounting remote ProcLens with 4-state proxies.

  M5: Fleet Treefilade Telemetry (D2)
  └── Deliverable: 50,000-node simulated fleet p99 aggregation in O(1).
```

### Milestone Acceptance Criteria

#### Milestone M0: Foundations (Week 1–2)
- [ ] All six spikes (S1–S6) pass and record benchmark artifacts.
- [ ] `Rankfilade` achieves $< 35\text{ ns}$ descent on 500,000 cells; `CellSlot` strictly 32 bytes.
- [ ] 9Z2026 wire codec serializes and deserializes all 12 message pairs with $O(1)$ stack allocations.
- [ ] CRDT-ZZ resolves multi-node concurrent link races with zero invariant violations.

#### Milestone M1: Synthetic OS Engine (Week 3–4)
- [ ] `ProcLens` projects host Linux `/proc` into `ArenaManifold`.
- [ ] Process hierarchy uses $d.\text{child}$ (firstborn) and $d.\text{sibling}$ (birth order).
- [ ] Command transactions on $d.\text{ctl}$ return synchronous error strings on invalid actions.
- [ ] 1,000 consecutive epoch tosses execute without memory leakage or pointer corruption.

#### Milestone M2: Transductive Enfilade Derivations (Week 5–6)
- [ ] `TransductiveView` derives ephemeral dimensions in `ViewManifold`'s derived arena.
- [ ] `Arrayfilade` predicate pushdown prunes $>95\%$ of unpromising subtrees in $O(1)$.
- [ ] `Spanfilade` stabs visible viewports in $< 20\ \mu\text{s}$, extracting all transclusions.

#### Milestone M3: Presentation Views (Week 7–8)
- [ ] `LatticeView`, `XanadocView`, and `AcmeView` implemented as pure `SliceView` layout algorithms.
- [ ] `Layoutfilade` enables smooth virtualized scrolling over 100,000 lines at 120 FPS ($8.33\text{ ms}$).
- [ ] Acme panes support fluid text tags, B1/B2/B3 chords, and active $d.\text{event}$ streaming.
- [ ] `tools/compare-backends.sh` passes across OpenGL, OpenGL ES, and Vulkan backends.

#### Milestone M4: Distributed Spool-Mesh Lenses (Week 9–10)
- [ ] Local `xuzz` mounts remote `ProcLens` over BitTorrent BEP 10 actor wire.
- [ ] Non-blocking 4-state proxy cells ensure UI never drops frames during network delays.
- [ ] Hypertime configuration rollback unwinds physical OS side-effects via `ReconcileLens`.
- [ ] `DimCap` tokens block unauthorized $d.\text{ctl}$ access.

#### Milestone M5: Enterprise Fleet Telemetry (Week 11–12)
- [ ] 50,000-node simulated cluster aggregates telemetry up a 4-level `Treefilade`.
- [ ] Root crum evaluates fleet p99 latency in $< 1\text{ ms}$.
- [ ] Simulated kernel crash dumps breadcrumbs into NVRAM, recovering post-mortem trace.
- [ ] `make check` and all unit test suites 100% green.

---

## 6. Risk Register & Mitigations

| Risk | Impact | Probability | Mitigation Trigger & Action |
| :--- | :--- | :--- | :--- |
| **CSR Compaction Cascades** | High | Medium | If sorting ranks by fluctuating scalars triggers frequent `ArenaManifold::compact()`, switch to in-place scalar value updates and perform sorting at presentation layout time via `Rankfilade`. |
| **DHT Swarm Churn Stalls** | High | High | If BEP 46 lookups exceed 1,000 ms, enforce direct `SocketChannel` (QUIC/TCP) multiplexed streams immediately following peer discovery. |
| **HarfBuzz Re-Shaping Lag** | Medium | Medium | If transductive pipelines trigger per-frame text shaping, mandate that `CellLayoutMetrics` are updated reactively on `onNodeUpdated()` only. |
| **Cycle Formation in Unions** | Critical | Low | Enforce that union dimensions are strictly read-only transductive views with cycle-bounded iterators (depth limit $\le 65,536$). |
| **Split-Brain Configuration Drift**| High | Medium | `ReconcileLens` must gate rollout progression behind cryptographic Canary Barrier cells signed by monitoring consensus. |

---

### Conclusion

This implementation plan translates the visionary philosophy of Plan 9 and Project Xanadu into an
executable, grounded engineering campaign. By rigorously enforcing repository layering, mathematical
manifold invariants, 9Z2026 wire protocol contracts, and Nelsonian enfilade acceleration, the
resulting Dimension-Cell Operating Environment will achieve unprecedented compositional elegance
without sacrificing industrial performance or operational reliability.
