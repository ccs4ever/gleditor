# Xudu-Zigzag Hypermedia Bridge: Next Implementation Stage

## Decision

The next stage is **production bridge composition and data-path hardening**. Stages 1–6 supplied
most of the types, algorithms, and UI pieces, but Xudu does not compose them at runtime: it never
supplies a manifold or cell-anchor resolver to `LinkBeams`. Consequently the running application
takes its document-only discovery path and cross-domain links, satelloids, cell drops, and the
background continuum are unreachable.

This stage must make the existing bridge real before expanding its visual vocabulary.

## Current-State Reconciliation

- Preserve the two transclusion representations, but name them consistently:
  `UniversalTransclusionPair` is the 64-byte compatibility/render object and
  `CompactTransclusionPair` is the 32-byte streaming object. Update the original bridge design and
  tests to say this explicitly. Do not use the current compact conversion for cell results until it
  carries endpoint kind, flags, and span index losslessly.
- Make `Spanfilade` the canonical discovery implementation. `link_layout.cpp` and
  `enfilade/spanfilade.cpp` currently duplicate pair assembly, and the production beam path calls
  the former. Retain a bounded linear reference implementation only in tests for differential
  checks.
- Keep `LinkBeams` decoupled from Zigzag. The Xudu shell may compose a bridge surface, but
  `LinkBeams` continues to receive only `Manifold` views and `CellAnchorResolver` callbacks.

## Work Packages

### 1. Define the bridge ownership and presentation seam

Extract an app-neutral Zigzag presentation surface from the standalone visualizer into
`apps/common/xanadu/`. It owns a synchronized `Manifold`, focus cell, visible-cell state, and cell
anchor computation; it binds to an existing store/version rather than copying a store.

Introduce a small bridge-facing interface exposing:

- `const Manifold &manifold() const`;
- current focus cell and configured radius;
- `optional<CellAnchor> cellAnchor(CellRef) const`;
- frame, pick, and accessibility participation; and
- explicit dirty/revision notification after store or focus changes.

Before implementation, make the source-of-truth decision explicit: the document store that records
the clasp link is the bridge's link authority, while the embedded manifold is rebuilt from the
selected structure-bearing store. If links are later allowed to span independently stored documents
and slices, add a federated link-source interface then; do not silently copy or merge stores here.

### 2. Compose the surface in Xudu

Create an Xudu-owned `BridgeCoordinator` that selects the active bridge store, synchronizes the
presentation surface, and owns its lifecycle. It must:

1. register the surface as a frame, pick, and accessibility contributor;
1. install its manifold/focus in `LinkBeams::setManifoldViews()`;
1. install `cellAnchor()` through `CellAnchorResolver`;
1. update the coordinator only at store, focus, layout-config, or viewport-radius invalidation
   boundaries, never by reconstructing the surface per frame; and
1. place the surface on the configured associative depth tier.

No Xudu source may include the standalone `apps/zigzag` application header. Both executables consume
the extracted common surface.

### 3. Complete real cross-domain interaction

Wire Zigzag cell selection and drag into `PouchDrawer::handleCellDrop()`. Construct each cell item
from its exact `Manifold::contentOf(cell)` spans, cell reference, and rank coordinate; links remain
ordinary immutable-span `OpKind::Link` records. Wire the existing drag guide and forging burst.

Add bidirectional callbacks:

- document link activation resolves a cell anchor, requests satelloid alignment, and preserves its
  native tether; and
- cell badge/Enter resolves the document range and asks Xudu to focus it.

The coordinator owns these callbacks, preventing either presentation component from depending on the
other.

### 4. Make discovery and staging production-grade

Replace the beam path's direct `placeTransclusions()` call with a cached `Spanfilade` snapshot.
Rebuild the snapshot only when visible document pieces, manifold contents, focus, or radius change.
Implement canonical overlap enumeration as a bulk sweep/range-stabbing operation with
`O(N log N + K)` construction/enumeration, where `K` is emitted overlap pairs; high overlap may
produce quadratic output and must be bounded by the configured cell radius.

Use the lossless compact representation for discovery/streaming only after its endpoint metadata is
complete. Expand to the compatibility object at rendering boundaries when legacy field access
requires it.

Cache formatting by format-link revision and dirty cells. `syncIncremental()` must not rescan every
cell and every format link after an unrelated operation. Reuse visible-cell traversal/staging
buffers and the resolver index so the `formatFlags == 0` path avoids resolver construction and
transient containers.

### 5. Turn looms and configuration into actual runtime policies

Stage each `TransclusionLoom` as one bundled GPU strip/instance range. Preserve member strands for
hover and picking, but do not submit one `band()` per member merely with reduced alpha. Precompute a
strand-to-loom lookup when strands are rebuilt.

Replace split and unused configuration with one atomically applied `BridgeRuntimeConfig` snapshot.
It includes cell radius, background depth/opacity, satelloid alignment/tether/mass/gap, tether
control depth and tessellation, and loom enablement/alpha. Populate it from Xudu's live system
xanadocs and Zigzag's system-slice schema/notes cells. Remove duplicate policy fields and migrate
remaining bridge literals to typed fallback defaults.

## Verification and Gates

1. Unit: lossless compact-pair round trip for document and cell endpoints; ABI assertions
   distinguish the 64-byte compatibility type from the 32-byte streaming type.
1. Differential: mixed document/cell transclusion results from the canonical Spanfilade match the
   retained reference scanner, including merged adjacent spans and radius pruning.
1. Headless integration: create a structure-bearing store and document, drag a real cell span into
   the pouch, forge a doc-to-cell link, and assert resolved strand, cell anchor, satelloid request,
   and reverse-focus callback.
1. Performance: benchmark high-overlap mixed contexts; repeated no-op and one-op formatting sync at
   2k/8k cells and 0/10/100 format links; assert no full-cell recomputation on the no-op path.
1. Rendering: assert `K` contiguous loom members stage as one bundle draw unit while each member
   remains pickable; record draw count and allocations in the dual-continuum path.
1. Configuration: mutate live Xudu layout values and Zigzag slice values, then assert the same
   snapshot changes discovery radius, surface depth, solver parameters, and loom behavior.
1. Run `make -j$(nproc) test`, `make format-check`, and `make lint` headlessly after initializing
   the required submodules.

## Exit Criteria

The bridge is complete for this stage when a normal Xudu session visibly hosts a synchronized Zigzag
continuum, can forge and navigate a real document-to-cell link without copying primedia, uses
Spanfilade rather than the duplicate scanner in its beam path, and demonstrates bundled looms and
configuration changes through automated headless tests.
