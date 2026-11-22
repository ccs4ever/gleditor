---
name: xanadu-engine
description: >-
  Xanalogical engine (apps/common/xanadu/), Store/CompactOpNode/Manifold/ArenaManifold/Vortex types,
  and system://* config. Use when modifying stores, permascroll, enfilades, publication, ZigZag
  cells, language runtimes, or design/ specs.
---

# Xanadu and ZigZag engine architecture

## Project structure and purity boundaries

- **`apps/common/xanadu/` — where the xanalogical engine lives.** `<common/xanadu/store.hpp>` is the
  include spelling, `apps/common/xanadu/store.cpp` is the file to edit. Subdirectories: `enfilade/`
  (Spanfilade, Chronofilade + `EdlTransform`, Holefilade, Arrayfilade — all ephemeral replay
  products that mint no operations), `identity/` (BEP 10 plugins, Hashcash PoW, network controller),
  `vortex/` (core, VM, host, stdlib), `vql/`, `vpl/`, `vprolog/`, `vlace/`, `vicon/`, `zigzag/`.
- **Engine purity**: `apps/common/xanadu/` must **never** include `apps/xuzz/`, `apps/common/ui/`,
  or a library header that needs a graphics device. Header-only, device-free library headers
  (`<gleditor/cpp26*.hpp>`, `<gleditor/spatial.hpp>`, `<gleditor/draw_budget.hpp>`) are allowed.
- **No `apps/xudu/` or `apps/zigzag/`**: do not create either. `xuzz` is the sovereign unified
  hypermedia application integrating Xanadu hypertext and ZigZag space; `xanadu` is the engine.
- `apps/common/ui/xanadoc/` — xuzz's xanadoc UI components (`beams.cpp`, `session.cpp`,
  `bridge_coordinator.cpp`, `views.cpp`, overlays; in `namespace xanadu`). They emit no Structure
  operations directly (only `batch_orchestrator.cpp` does); the editor is text-ops only.
- `apps/common/ui/slice/` — slice visualizer (`zigzag_visualizer.cpp`,
  `unified_transclusion_engine.cpp`, `zigzag_commands.cpp`).
- `apps/common/ui/` — shared xanalogical UI (`hypertime_graph.cpp`, `quotation_builder_overlay.cpp`,
  `store_object_manager.cpp`, link-panel and page-break presentations).
- `apps/xuzz/` — the program: `main.cpp`, `xuzz_app.cpp`, `cli.cpp`, and `view_coordinator.cpp`.
- `apps/vquery`, `vqueryc`, `vpl`, `vplc`, `vprolog`, `vlace`, `vlacec`, `vicon`, `viconc` — query
  and language front ends over the shared engine.
- There is no `assets/zigzag/` any more: the YAML slice format is deleted and every slice is a
  store.
- `tools/` — `compare-backends.sh`, `create-sample-xanadocs.sh`, `create-floating-image-sample.sh`,
  `swarm-netns-test.sh`, `xudu-e2e-orchestration.sh`, `check-config-harmony.sh`,
  `benchmark-kjv-load.{py,sh}`, `layout-latency-probe.cpp`, `shader_assemble.cpp`, `store-dump.cpp`,
  `xudu-swarm-peer.cpp`, `code-quality-audit.py`, and scene/showcase generators.

## Load-bearing engine types

| Header / Type                                   | Role & Key Invariants                                                                                                                                                                                                                                                                                                                 |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `store.hpp/.cpp`                                | OSMIC hypertime (branches, microversions, six hyperops) & Structure API (`sliceGenesis`, `makeCell`, `makeScalarCell`, `setLink`, `spliceCell`, `spliceCellSpan`, `setValue`, `makeDimension`, `rebuildManifold`).                                                                                                                    |
| `ops.hpp`                                       | `OpKind::Structure` (MAKE/CHANGE STRUCTURE MAP): verb (`MakeCell`/`SetLink`/`SetValue`/`Splice`), direction & `ValueKind` in `CompactOpNode::flags`. `Store::replay()` treats it as a text no-op. `SetLink`'s `sourceOpIndex` chains previous ops on the cell back to `MakeCell` (`CellRef`, R7).                                     |
| `compact_op.hpp`                                | 64-byte `CompactOpNode`, cache-line aligned, immutable once stored (`PROT_READ` sealed segments; tree edges in `SegmentedOpsSpool::tree`). `static_assert`s: size, alignment, `offsetof(value) == 56`.                                                                                                                                |
| `segmented_ops_spool.hpp/.cpp`                  | `mmap`'d ops tree; 64 KiB Merkle-piece-aligned segments (1,024 nodes each), sealed segments, id hash, `TreeLinks`.                                                                                                                                                                                                                    |
| `user_permascroll.hpp/.cpp`                     | Author's single permascroll — **only place primedia is stored**; a `Store` is handed one, never makes its own.                                                                                                                                                                                                                        |
| `store_tables.hpp`                              | `store.tables` (bencode v4): local segments, ordered private deployment scroll descriptors with imported author-local scroll binding. Registry, `Link` records, editions & annotations are Structure ops; deployment slots have no hypertime names.                                                                                   |
| `publication.hpp/.cpp`, `provenance.hpp`        | Publish/adopt (`globalise`/`localise`, `opRefOf`/`localiseOpRef`, `historyFromSeal`); Ed25519 provenance in `AUTHORSHIP.tsv` + X.509 delegation sealed into torrent. See `merkle_ledger`, `managed_torrent`, `transcopyright_*`, `swarm*`.                                                                                            |
| `zigzag/manifold.{hpp,cpp}`                     | Slice model: cells folded from Structure ops, `CellRef` = ops index, links = per-cell CSR runs keyed by dimension cell, content = run of spans. `rebuildManifold()`, `advance()`, `verifyAgainstFullRebuild()`. Typing into a xanadoc mints no cells.                                                                                 |
| `zigzag/arena_manifold.{hpp,cpp}`               | Ephemeral half of R8 split: same cells/runs, no ops, refs carry `ephemeralBit` (refused by `Store::setLink`/`applyStructure`). `mark()`/`release()`/`discard()`, `compact()` refused with active mark, `promote()` maps refs to persist. CoW view over base `Manifold`; `scratchScroll` refused elsewhere. Hosts Vortex/VQL/VPL/Vlog. |
| `zigzag/vlog.{hpp,cpp}`, `vortex/vortex_vm.hpp` | `unify()` and trail over the arena; logic opcodes (`Unify`, `Choice`, `Fail`, `Cut`, `ChoicePoint`) driving `vprolog`.                                                                                                                                                                                                                |
| `zigzag/zz_xudu_projector.{hpp,cpp}`            | Xanadoc ↔ ZigZag: `projectXuduToZigzag` (`d.doc`/`d.transclude`/`d.link`/`d.version`), `sliceToStore()`, `storeToSlice()`. `role`/`mime_type`/`media_path` live on `d.role`/`d.mime`/`d.media`.                                                                                                                                       |
| `zigzag/compact_zzcell.hpp`                     | **Dead type**: pre-manifold cell layout (`static_assert` ≤ 1024B), referenced by nothing but itself. Prefer `zigzag::CellSlot` (32B + 12B/dim).                                                                                                                                                                                       |
| `scalar.hpp/.cpp`                               | Scalar cell's two halves (R6): shortest-round-trip text spooled as primedia + canonical bits in `value` (equality only, never addresses). sNaN refused; `makeScalarCell()` is not a `makeCell()` overload.                                                                                                                            |
| `system_docs.hpp/.cpp`                          | Five system xanadocs (`system://keymap\|settings\|layout\|ui\|pouches`) over 10 dims (`d.dims`, `d.vars`, `d.values`, `d.groups`, `d.subgroups`, `d.clone`, `d.notes`, `d.schemas`, `d.alternates`, `d.default`) with schema validation; heaviest Structure emitter. Adding a setting requires a `defaultSettingSpecs()` entry.       |
| `multi_store.hpp`                               | Composes many stores in **one arena** via `d.stores`; foreign cells attached and proxied in composite arena.                                                                                                                                                                                                                          |
| `store_loader.hpp`                              | Singular native store loading (`loadStore`) and file import pathways (`importFileStore`).                                                                                                                                                                                                                                             |
| `vortex/vortex_host.hpp`                        | `promoteAndAttachToStore()` persists a compiled program as real Structure operations (called in production by the zigzag VQL palette).                                                                                                                                                                                                |

## Xanadulogical and identity architecture

- **One permascroll per user.** All typing appends to one `UserPermascroll` bound to the author's
  Ed25519 identity (anchored to the master identity fingerprint), in 64 KiB page-aligned segments
  (Merkle piece stability, zero-copy `mmap(MAP_FIXED)` growth). A `Store` is an edit decision list
  over slot 0 (local) or external scrolls (`ScrollId > 0`). `DeviceDelegation` maps master Ed25519
  identities to device keys via standard X.509 v3 certificates (`StandardCryptoEngine`).
- **Merkle identity ledger.** `MerkleLedger` (merklecpp) holds verified 64-hex SHA-256 digests of
  Ed25519 public keys and email mappings; BEP 10 plugins `xanadu_identity_lookup`,
  `xanadu_oracle_vote`, `xanadu_oracle_verify` do challenge-response and peer gating;
  `HashcashEngine` PoW resists Sybil/DoS. For wire protocol, BEP 46, and torrent internals, load
  [`bittorrent-expert`](../bittorrent-expert/SKILL.md).
- **Transclusion discovery** is the Spanfilade ($O(\log N + K)$ interval stabbing over permascroll
  addresses, across documents and cells); `link_layout.cpp` still holds a duplicate linear scanner
  the bridge plan retires.

## ZigZag cell representation and system store governance

- **Structure over prose:** Text that would require parsing or mixed-media cells belong in
  additional cells and dimensions (`d.parsed`, `d.media`, `d.roles`, etc.) rather than embedded
  markup inside a cell's canonical content span — keeping cells orthogonal, quotable, and free of
  grammar-lock.
- **Xanadocs in `xuzz` (`system://*`):**
  - Read all user-configurable values (physics springs, beam tensions, anchor colours, notification
    offsets, keymaps) **live dynamically** from `system://keymap`, `system://settings`,
    `system://layout`, `system://ui`, or `system://pouches` — no naked magic numbers.
  - **Zero Markdown syntax:** system xanadocs and companion pages must not use markdown (`#`, `**`,
    `_`, etc.).
  - **Native format links:** page and section headers (`Notes`, `Schema`) must use native Xanadu
    format links (`LinkType::Format`: `FormatAttribute::Bold`, `FormatAttribute::AlignCentre`, and
    font scale specifiers).
  - **Required supplemental metadata:** every system xanadoc must be bidirectionally xanalinked to
    (1) a **Schema & Purpose Page** (role, fields, data types, physical units, fallback defaults)
    and (2) a dedicated **User Notes Page** headed with `Notes` (bold, centered, larger format
    links).
- **ZigZag slices in `xuzz`:**
  - Always backed by a sovereign `xanadu::Store` (`CompactOpNode`, `OpKind::Structure`,
    `Store::rebuildManifold()`), never legacy YAML slice files.
  - Configurable slice parameters (cell spacing, camera projections, cycler speeds) resolve
    dynamically from **system zigzag slices** backed by sovereign stores.
  - No markdown syntax in cells; metadata links along orthogonal dimensions: `d.schemas` (purpose
    and schema) and `d.notes` (user notes).

For Xuzz link navigation (`system://activity`, endpoint browsing) and user-seat journey validation,
load [`xuzz-ui-design`](../xuzz-ui-design/SKILL.md) and
[`ux-validation`](../ux-validation/SKILL.md).

## References

- On-disk formats, retired filenames, `store-dump`, and `Session::systemStoreIndex()`:
  [references/on-disk-formats.md](references/on-disk-formats.md)
- Annotated `design/` document map ("Where the reasoning lives"):
  [references/design-map.md](references/design-map.md)
- Architecture overview of Xanadu, ZigZag, swarm publication, and Vortex languages:
  [docs/xanadu.md](../../../docs/xanadu.md)
