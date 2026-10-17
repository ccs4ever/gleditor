# The Anything-to-ZigZag Lens: Generalizing the Xanadu Projector into a Bidirectional Source Lens

A design specification for generalizing the Xanadu-to-ZigZag translator
([`zz_xudu_projector.{hpp,cpp}`](apps/common/xanadu/zigzag/zz_xudu_projector.hpp)) into a
source-agnostic, bidirectional **lens**: a trait that projects arbitrary external structure into a
ZigZag manifold and unprojects edits back to that structure's own operations. The existing xanadoc
projector becomes one implementation; a filesystem-tree lens is the second, built to force the
abstraction to be real rather than xanadoc-shaped.

Nothing here is wired into the build. This document records the model, the invariants it must
preserve, the proposed interface, a worked filesystem example, the identity-mapping scheme, a
round-trip fidelity analysis, and a phased plan. Every claim about current behaviour cites a
`file:line` found by reading the code. It is a design deliverable only — no lens is implemented.

______________________________________________________________________

## 1. Problem Statement

The engine already turns one external structure — a Xanadu store's documents, branches, and links —
into a ZigZag manifold, and back. That translator is hard-wired to xanadocs in three ways:

- its *source* is a `xanadu::Store` plus a list of `MicroversionId`s
  ([`zz_xudu_projector.hpp:92`](apps/common/xanadu/zigzag/zz_xudu_projector.hpp));
- its *dimensions* are the xanadoc-specific `d.doc` / `d.transclude` / `d.link` / `d.version` /
  `d.clone` set, baked into `XuduProjectorOptions`
  ([`zz_xudu_projector.hpp`](apps/common/xanadu/zigzag/zz_xudu_projector.hpp), the
  `XuduProjectorOptions` struct);
- its *projection logic* is paragraph-splitting and span-overlap heuristics specific to concatext
  ([`zz_xudu_projector.cpp:43`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp),
  `projectXuduToZigzag`).

Yet the manifold is a general multidimensional cell space, and the convergence
([`store-slice-convergence.md`](store-slice-convergence.md)) already treats *any* authored structure
— settings, keymaps, logic clauses, array grids — as cells on dimensions. There is no reason the
only *external* thing that can become cells is a xanadoc. A filesystem tree is structure: a
directory is a container, a file is a leaf, each has metadata (size, mtime, permissions, MIME), and
all of it has an obvious rank layout (`d.dir` for parent/child, `d.sibling` for ordering, `d.size` /
`d.mtime` / `d.perm` / `d.mime` for metadata). Projecting it into the manifold lets the same
visualizer, the same VQL/VPL/Vlog runtime, and the same navigation that work over xanadocs work over
a directory tree — and unprojecting edits (a cell renamed, a cell moved between ranks, a metadata
cell changed) lets a ZigZag gesture become a `rename(2)` / `chmod(2)` / `utime(2)`.

The goal: factor the *source-specific* half of the projector behind a `ProjectionLens` trait so that

1. the existing xanadoc projector is re-expressed as `XanadocLens` with **zero behaviour change**
   (every checked-in fixture round-trips identically, `xudu-dump --section=ops` byte-identical);
1. a new `FilesystemLens` projects a directory tree with metadata and unprojects edits to real
   filesystem operations;
1. the hard problem — a filesystem has no append-only ops index, so a `CellRef` cannot be its stable
   identity the way R7 makes an ops index the xanadoc cell's identity — is solved by an explicit,
   lens-owned **external-key ↔ CellRef** mapping rather than by pretending the two spaces coincide.

______________________________________________________________________

## 2. Evidence: How the Current Projector Works

Read in full: `zz_xudu_projector.{hpp,cpp}`, `manifold.hpp`, `arena_manifold.hpp`,
`zzstructure.hpp`, the Structure API in `store.hpp`, and `store-slice-convergence.md` §3–§5, R7, R8.

### 2.1 Two translators, two source shapes, two targets

There are, confusingly, **two** translation surfaces in the one file, and the lens must subsume
both:

- **The DTO projector.** `projectXuduToZigzag()`
  ([`zz_xudu_projector.cpp:43`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)) takes a
  `std::vector<XuduDocInput>` and `std::vector<xanadu::Link>` and produces a `ZzStructureDocument` —
  the in-memory DTO ([`zzstructure.hpp`](apps/common/xanadu/zigzag/zzstructure.hpp),
  `ZzStructureDocument`). It reads assembled *text* and *spans*, not a `Store`.
  `projectStoreToZigzag` / `projectStoreWithProvenance`
  ([`zz_xudu_projector.cpp:343`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)) are thin wrappers
  that first `store.rebuild(verId)` each version, assemble its text through
  `store.read`/`store.resolve`, and hand the result to the DTO projector. The target here is the
  DTO, not a `Manifold`.
- **The convergence translators.** `sliceToStore()`
  ([`zz_xudu_projector.cpp:548`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)) mints a
  `ZzStructureDocument` into a `xanadu::Store` **as `OpKind::Structure` operations** via the
  Structure API, and `storeToSlice()`
  ([`zz_xudu_projector.cpp:678`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)) folds a
  `Manifold` back into a DTO. These are the real cell-minting path; the DTO projector is the legacy
  paragraph-splitting path that step 20 of the convergence is retiring.

### 2.2 How documents/branches/links become cells on dimensions

In the DTO projector:

- **`d.doc` — reading flow.** Each paragraph (or whole document) becomes a `Cell`; sequential cells
  within one document are linked `pos`/`neg` on `opts.doc_dimension`
  ([`zz_xudu_projector.cpp`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp), the `docCellChains`
  loop under comment `--- 1. Link sequential reading order`).
- **`d.transclude` — shared spans.** Cells in *different* documents whose spans overlap
  (`spansOverlap`, [`zz_xudu_projector.cpp:28`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp))
  are linked on `opts.transclusion_dimension` (`--- 2. Link transclusions`).
- **`d.version` — microversion lineage.** Positionally-aligned cells across consecutive documents
  are linked on `opts.version_dimension` (`--- 3. Link sequential microversion steps`).
- **`d.link` — xanalinks.** Each `xanadu::Link`'s left/right endsets are matched to cells by span
  overlap and linked on `opts.link_dimension` (`--- 3. Link Xanalinks`).
- **`d.clone` — unchanged clones.** A paragraph whose span was already seen is minted as a
  content-free clone (`cell.data = ""`, `cell.role = "xudu_clone"`) linked on `opts.clone_dimension`
  to its predecessor (the `isUnchangedClone` branch).

In the convergence translator `sliceToStore()`, `role` / `mime_type` / `media_path` do **not** ride
in `CompactOpNode` fields — they become **cells on their own ranks** `d.role` / `d.mime` / `d.media`
([`zz_xudu_projector.cpp:37`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)), attached posward of
the cell they describe (the `attach` lambda in `sliceToStore`). `storeToSlice()` reads them back off
those ranks and classes the attribute cells and the dimension cells as *structure* (the `structural`
set in `storeToSlice`,
[`zz_xudu_projector.cpp:700`-ish](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)), so a round trip
does not re-emit them as content. This is R13's "metadata is cells on a dimension" and is exactly
the mechanism a filesystem lens needs for file metadata.

### 2.3 Which direction is lossy

Both directions lose something, and the losses are different:

- **Project (source → cells), lossy.** A paragraph straddling two pieces is addressed by its first
  part only, because a cell holds one span — the code says so in a comment: "Clamped to the piece
  the paragraph starts in, because a cell holds one span … a truncation, not a misdirection"
  ([`zz_xudu_projector.cpp`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp), inside the
  `split_by_paragraphs` branch). `storeToLinkPackage()` publishes a multi-span cell "under the
  address of its first" ([`zz_xudu_projector.cpp`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp),
  the `contentRun.front()` comment).
- **Unproject (cells → DTO), lossy on ids.** `storeToSlice()`'s own doc comment: "a CellRef is an
  operation index and is used directly as the YAML id, so a round trip renumbers cells rather than
  restoring whatever numbers the original file used"
  ([`zz_xudu_projector.hpp`](apps/common/xanadu/zigzag/zz_xudu_projector.hpp), `storeToSlice` doc
  comment). Scene metadata, dimension colours, and the `view` axis binding are **not** reconstructed
  by `storeToSlice()` — it emits only cells, their data, role/mime/media, and dimension links.

### 2.4 Whether storeToSlice / sliceToStore form a real round trip

They form a real round trip **up to cell-id renumbering and presentation metadata**, and this is
tested: the convergence records that "the real `assets/zigzag/zigzag_structure.yaml` round-trips …
every cell's text, role, MIME type and posward rank compares equal through the id mapping, with no
refused operations and `verifyAgainstFullRebuild()` holding"
([`store-slice-convergence.md`](store-slice-convergence.md), §9 step 20). The id renumbering is
intrinsic: `sliceToStore()` maps each DTO `CellID` to the `CellRef` of the operation that minted it
(`out.cells.emplace(id, store.cellRefOf(out.version))`,
[`zz_xudu_projector.cpp`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)), so the DTO's own ids do
not survive — the cell's identity becomes its name in hypertime (R4, R7). This is the single most
important fact for the generalized lens: **the engine side owns identity as an ops index; the source
side's own identity is a separate space the lens must map.**

### 2.5 Where projection reads and where it writes

- **DTO projector reads** assembled text + spans from `Store::rebuild` / `read` / `resolve`
  ([`zz_xudu_projector.cpp:343`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)) and **writes** a
  `ZzStructureDocument` DTO.
- **`sliceToStore()` reads** a DTO and **writes** persistent `Manifold` state by calling the
  Structure API on a `xanadu::Store`: `sliceGenesis`
  ([`store.hpp:446`](apps/common/xanadu/store.hpp)), `makeCell` / `makeScalarCell`
  ([`store.hpp:466`](apps/common/xanadu/store.hpp)), `setLink`
  ([`store.hpp`](apps/common/xanadu/store.hpp)), `makeDimension` / `DimensionRegistry::getOrCreate`
  ([`dimension_registry.hpp:115`](apps/common/xanadu/zigzag/dimension_registry.hpp)), then
  `store.rebuildManifold()` ([`store.hpp:413`](apps/common/xanadu/store.hpp)) to fold.
- **`storeToSlice()` reads** a folded `Manifold`
  ([`manifold.hpp`](apps/common/xanadu/zigzag/manifold.hpp), `cells()` / `dimensionsOf()` /
  `textOf()` / `linked()`) and **writes** a DTO.

The convergence path therefore writes through **two** targets depending on persistence intent: the
**persistent `Manifold`** via `Store`'s Structure verbs (every write mints a `CompactOpNode`,
earning a name in hypertime), or the **ephemeral `ArenaManifold`** via its no-op write path
(`makeCell` / `link` / `setContent` / `promote`,
[`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp)). The lens must be able to
target either, and the arena is the natural staging ground for a *diff* (see §6).

______________________________________________________________________

## 3. Invariants the Generalized Lens Must Preserve

These are non-negotiable; a lens that violates any is wrong by construction. Each is cited to the
code that enforces it.

1. **Cell identity is the operation that minted it (R7).** On the persistent side a `CellRef` *is*
   an ops-spool index, and a `SetLink` names its subject by chaining through `sourceOpIndex` to the
   `MakeCell` whose index is the `CellRef`
   ([`manifold.hpp`](apps/common/xanadu/zigzag/manifold.hpp), `CellSlot::birthOp` / `lastOp` doc
   comments; [`store-slice-convergence.md`](store-slice-convergence.md) R7). A lens may **not**
   invent its own cell numbering on the persistent side.
1. **Links are bidirectional — one edge, two ends.** `Store::setLink` "breaks whatever either end
   was holding along `dim`" ([`store.hpp`](apps/common/xanadu/store.hpp), `setLink` doc comment),
   and the fold maintains both ends ([`manifold.hpp`](apps/common/xanadu/zigzag/manifold.hpp),
   `Manifold::link` doc comment). A lens emits posward links only, plus any negward link the source
   does not state reciprocally — exactly as `sliceToStore()` does
   ([`zz_xudu_projector.cpp`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp), the "Posward links
   only" loop).
1. **A cell exists only where an operation minted one.** Typing into a xanadoc mints no cells
   ([`store-slice-convergence.md`](store-slice-convergence.md) §3, R12 `d.meta-dims` is *generated*
   not stored). A lens projecting a source into the persistent manifold mints one `MakeCell` per
   source node and nothing implicit.
1. **`ephemeralBit` refusal on persistent writes.** `ephemeralBit = 0x8000'0000`
   ([`manifold.hpp`](apps/common/xanadu/zigzag/manifold.hpp)); `Manifold::applyStructure()` refuses
   a `SetLink` whose target `isEphemeral()` (`FoldRefusal::EphemeralTarget`), and `Store::setLink`
   throws on an ephemeral ref ([`store.hpp`](apps/common/xanadu/store.hpp), `setLink` `@throws`). An
   arena ref must never escape into a persistent operation except through `promote()`.
1. **`promote()` is the one crossing to the spool.** Only what is reachable from the promoted root
   is written, scratch spans acquire permanent addresses, deferred scalars are rendered, and refs
   are *mapped* rather than trusted
   ([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp), `promote()` doc comment;
   [`store-slice-convergence.md`](store-slice-convergence.md) R8). A lens that stages in an arena
   crosses to persistence only here.
1. **Determinism.** `sliceToStore()` mints cells and dimensions in sorted order so "one document
   always produces one operation sequence"
   ([`zz_xudu_projector.cpp`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp), the
   `std::ranges::sort` on `ids`;
   [`zz_xudu_projector.hpp`](apps/common/xanadu/zigzag/zz_xudu_projector.hpp), `sliceToStore` doc
   comment). A lens must impose a total order on source nodes so a fixture can be regenerated and
   diffed.
1. **R8 — view movement is not an edit.** Navigating the projected manifold mints nothing in the
   source. Only a *user edit* to the structure (rename, move, metadata change) unprojects to a
   source operation ([`store-slice-convergence.md`](store-slice-convergence.md) R8).

______________________________________________________________________

## 4. The Proposed Interface: `ProjectionLens`

The lens is a trait with two halves. The **project** half turns a source into arena cells; the
**unproject/diff** half turns a changed arena into source operations. It owns one piece of state the
engine deliberately does not: the mapping between the source's own stable keys and the manifold's
`CellRef`s (§5).

The lens targets an `ArenaManifold` for both directions. This is deliberate and follows the engine's
own grain: the arena is the pure, no-op, copy-on-write write path
([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp)); projecting into it mints no
hypertime, diffing against its COW base is cheap (§6), and persisting is the single `promote()`
crossing (invariant 5). A lens that wants persistence promotes; a lens that only wants a live view
(the filesystem browser) never does.

### 4.1 Core types

```cpp
// apps/common/xanadu/zigzag/projection_lens.hpp  (proposed)
namespace zigzag {

/// A source node's own stable, lens-defined identity. Opaque to the engine.
/// For a xanadoc this is a (documentId, MicroversionId, PrimediaSpan) triple;
/// for a filesystem it is the inode-or-path key of §5. The lens is the only
/// code that knows how to compute, compare, and persist one.
using ExternalKey = std::string;   // canonical, stable, lens-owned encoding

/// One generic dimension every lens speaks, named once so a visualizer and a
/// query do not have to learn each source's private vocabulary. A lens maps
/// its source-specific ranks onto these where they fit and declares its own
/// where they do not (§4.3).
enum class GenericDim : std::uint8_t {
  Sequence,   ///< d.seq      -- ordered siblings / reading flow
  Containment,///< d.contains -- parent -> child (document->paragraph, dir->entry)
  Reference,  ///< d.ref      -- a link / cross-edge (xanalink, symlink)
  Clone,      ///< d.clone    -- shared-content identity (R12 master chain)
};

/// A single projected node, before it is minted. The lens fills this; the
/// framework mints it and records the ExternalKey <-> CellRef mapping.
struct SourceNode {
  ExternalKey key;                       ///< stable source identity (§5)
  std::variant<std::string, double, bool,
               std::span<const xanadu::PrimediaSpan>> content; ///< text / scalar / addressed span
  std::vector<std::pair<std::string, std::string>> attributes; ///< rank name -> value, e.g. {"d.mime","image/png"}
  std::uint64_t order{0};                ///< total order within siblings (invariant 6)
};

/// One edge the lens asserts, in terms of ExternalKeys (source-space), so the
/// framework can translate both ends through the mapping without the lens
/// touching a CellRef.
struct SourceEdge {
  ExternalKey from;
  ExternalKey to;
  std::string dimension;                 ///< rank name, e.g. "d.contains"
  DimVector dir{DimVector::POS};
};

} // namespace zigzag
```

### 4.2 The trait

```cpp
namespace zigzag {

/// A bidirectional translator between some external structure and a ZigZag
/// manifold. The xanadoc projector is one implementation; a filesystem tree is
/// another. The framework (ProjectionSession, §4.4) owns the arena and the
/// identity map; the lens owns only the source-specific halves.
class ProjectionLens {
public:
  virtual ~ProjectionLens() = default;

  // -- source -> cells -------------------------------------------------------

  /// Dimensions this lens needs, in mint order, each as (name, GenericDim or
  /// Custom). Declared up front so the framework can mint them deterministically
  /// before any node references one. Maps onto sliceToStore()'s dimension
  /// pre-mint pass (zz_xudu_projector.cpp, the `dimensionNames` set).
  [[nodiscard]] virtual std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const = 0;

  /// Enumerate every source node, in a total order (invariant 6). Pull-based so
  /// a huge tree streams rather than materialising. The framework mints a cell
  /// per node and records key -> CellRef.
  virtual void forEachNode(
      gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const = 0;

  /// Enumerate every edge, in ExternalKey space. Called after all nodes are
  /// minted, so both ends resolve through the map.
  virtual void forEachEdge(
      gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const = 0;

  // -- cells -> source (unproject / diff) ------------------------------------

  /// Apply one detected change back to the real source. The framework computes
  /// the diff (§6) and hands the lens typed deltas; the lens turns each into a
  /// source operation (a rename(2), a setLink in a xanadu::Store, etc.).
  ///
  /// Returns the ExternalKey the change now lives under -- which may differ from
  /// the input key (a rename changes a path-based key), so the framework can
  /// repair the mapping without the lens reaching into it.
  [[nodiscard]] virtual std::expected<ExternalKey, std::string>
  applyContentChange(const ExternalKey &node, std::string_view newContent) = 0;

  [[nodiscard]] virtual std::expected<ExternalKey, std::string>
  applyAttributeChange(const ExternalKey &node, std::string_view rank,
                       std::string_view newValue) = 0;

  /// A cell moved from one rank-neighbour to another: for a filesystem this is a
  /// move between directories; for a xanadoc a rearrange. `oldParent` is nullopt
  /// for a new root.
  [[nodiscard]] virtual std::expected<ExternalKey, std::string>
  applyReparent(const ExternalKey &node, std::optional<ExternalKey> oldParent,
                std::optional<ExternalKey> newParent, std::uint64_t newOrder) = 0;

  [[nodiscard]] virtual std::expected<ExternalKey, std::string>
  applyCreate(const SourceNode &node, std::optional<ExternalKey> parent) = 0;

  [[nodiscard]] virtual std::expected<void, std::string>
  applyDelete(const ExternalKey &node) = 0;
};

} // namespace zigzag
```

The `std::expected<ExternalKey, std::string>` return is load-bearing: an unproject can **fail**
(permission denied, a path that no longer exists), and it can **relocate the key** (a rename). Both
must flow back to the framework so the identity map stays consistent, and neither may be a silent
sentinel — this is the house rule "no sentinels outside the wire and on-disk formats"
([`AGENTS.md`](AGENTS.md), code style). `applyContentChange` taking a `std::string_view` rather than
a `CellRef` keeps the lens on the source side of the boundary; the framework resolves the cell.

### 4.3 Generic vs source-specific dimensions

| dimension      | generic? | xanadoc meaning               | filesystem meaning              |
| -------------- | -------- | ----------------------------- | ------------------------------- |
| `d.contains`   | generic  | document → paragraph          | directory → entry               |
| `d.seq`        | generic  | reading order (`d.doc`)       | sibling order within a dir      |
| `d.ref`        | generic  | xanalink (`d.link`)           | symlink / hardlink target       |
| `d.clone`      | generic  | shared-span clone (`d.clone`) | identical-content files (dedup) |
| `d.role`       | generic  | `role` attribute cell         | `file`/`dir`/`symlink` kind     |
| `d.mime`       | generic  | MIME of media cell            | libmagic MIME of file           |
| `d.transclude` | xanadoc  | overlapping primedia spans    | — (no analogue)                 |
| `d.version`    | xanadoc  | microversion lineage          | — (no analogue)                 |
| `d.size`       | fs       | —                             | file size in bytes (scalar)     |
| `d.mtime`      | fs       | —                             | mtime (scalar / ISO-8601 cell)  |
| `d.perm`       | fs       | —                             | mode bits (scalar)              |

The four `GenericDim` values are the vocabulary a visualizer and a query can rely on across every
lens. `d.role` / `d.mime` are generic *attribute ranks* already named in the engine
([`zz_xudu_projector.cpp:37`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)); the lens populates
them through `SourceNode::attributes`. Everything else is declared per lens via `dimensions()`.

### 4.4 The framework: `ProjectionSession`

The one genuinely new piece of C++ (§8). It owns the arena, the identity map, and the mint/diff
loops that are *identical* for every lens:

```cpp
namespace zigzag {

class ProjectionSession {
public:
  /// Project `lens` into a fresh arena (optionally over a base Manifold for
  /// COW). Mints dimensions, then nodes, then edges, recording the identity
  /// map. Deterministic given a deterministic lens.
  ProjectionSession(std::unique_ptr<ProjectionLens> lens,
                    const Manifold *base = nullptr);

  [[nodiscard]] const ArenaManifold &manifold() const noexcept;
  [[nodiscard]] ArenaManifold &manifold() noexcept;

  /// Resolve either direction of the identity map.
  [[nodiscard]] std::optional<CellRef> cellFor(const ExternalKey &) const noexcept;
  [[nodiscard]] std::optional<ExternalKey> keyFor(CellRef) const noexcept;

  /// Diff the arena against its COW base (or against the snapshot taken at
  /// construction when there is no base), classify every dirty cell (§6), and
  /// drive the lens's apply*() methods in dependency order (creates before the
  /// links that need them, deletes last). Repairs the identity map from each
  /// returned ExternalKey. Returns per-change results so a caller can report
  /// partial failure.
  struct ChangeReport { ExternalKey key; std::expected<ExternalKey, std::string> result; };
  [[nodiscard]] std::vector<ChangeReport> commit();

  /// Persist the arena into a Store instead of (or as well as) back to the
  /// source: the single promote() crossing (invariant 5). For a lens whose
  /// source is itself a Store this is a no-op; for the filesystem lens this is
  /// how a directory tree becomes a durable xanadoc slice.
  [[nodiscard]] std::optional<Promoted>
  promoteInto(xanadu::Store &store, const MicroversionId &parent, CellRef root);

private:
  std::unique_ptr<ProjectionLens> lens_;
  ArenaManifold arena_;
  std::unordered_map<ExternalKey, CellRef> keyToCell_;
  std::unordered_map<CellRef, ExternalKey> cellToKey_;
  // snapshot for base-less diff: the arena's state right after projection.
};

} // namespace zigzag
```

`commit()` is where invariants 2, 4, 5, 7 are enforced *once* rather than per lens: it never hands a
lens an `ephemeralBit`-tagged ref (the lens works in `ExternalKey` space), it computes the diff
against the COW base so navigation-only changes produce no deltas (R8), and it is the only place
that decides create/relink/delete ordering.

______________________________________________________________________

## 5. The Identity-Mapping Scheme (the Hard Problem)

On the xanadoc side identity is free: a `CellRef` is an ops index and the operation *is* the cell
(R7). A filesystem has no append-only ops spool, so there is no index that is both stable and
engine-native. The lens must therefore maintain an explicit, bidirectional map between a **stable
external key** and the arena `CellRef`, and the key must survive the operations the source supports
(rename, move, edit).

### 5.1 Why a path is not a key

The obvious key — the file's path — is wrong, because the headline unproject operation (**rename**)
*changes the path*. If the key were the path, a rename would orphan the mapping: the cell the user
renamed would no longer resolve, and a second edit to the "same" node would mint a duplicate. The
map must be keyed on something a rename does **not** change.

### 5.2 The scheme: `(device, inode, generation)` with a path fallback

For a filesystem that exposes inodes (Linux/macOS/BSD), the stable key is the pair `st_dev` ×
`st_ino` from `stat(2)`, which a rename and a move-within-a-filesystem preserve:

```
ExternalKey (filesystem) ::= "fs:" <st_dev> ":" <st_ino>              -- inode-stable
                           | "fsp:" <sha256(canonical_path)>          -- fallback, no inode
```

- **Rename / move within a device.** `st_dev`/`st_ino` are unchanged, so the key is unchanged and
  the map needs no repair — only the *path attribute cell* (`d.name` / content) changes. This is why
  `applyReparent` / `applyContentChange` return an `ExternalKey`: for the inode scheme they return
  the *same* key; the framework learns there is nothing to repair.
- **Cross-device move.** `rename(2)` fails across devices; the operation is a copy+delete, which is
  a new inode and therefore a new key — the lens returns the new key and the framework repairs
  `keyToCell_` / `cellToKey_` to point the existing `CellRef` at it. The cell keeps its identity;
  its key is rebound.
- **Hardlinks.** Two paths, one inode → one key → one cell. The lens emits one `SourceNode` per
  inode and the two paths become two `d.name` siblings or two `d.ref` edges, which is *correct*: a
  hardlink genuinely is one object with two names, and the manifold's shared-identity machinery
  (`d.clone`, R12) models exactly that.
- **No-inode fallback (FAT, some network mounts, a plain `std::filesystem` walk).** Hash the
  canonical path. A rename then *does* change the key, so the lens must detect a rename as a
  delete+create unless the gesture itself carried the intent. This is a genuine fidelity loss,
  recorded in §7.

The key is deliberately a `std::string` so one `ProjectionSession` can host **mixed** lenses in one
arena via `d.stores` ([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp),
`attach(Space)`): a xanadoc key (`"xd:<documentId>:<microversion>:<span>"`) and a filesystem key
(`"fs:<dev>:<ino>"`) never collide, so a directory and a xanadoc can be browsed in one space.

### 5.3 Mapping across the persistent crossing

When a lens *promotes* into a `Store` (invariant 5), the arena `CellRef` becomes a real ops index
and the external key must be recorded so a later session can re-find the same cell. Two options, and
the second is recommended:

- **(a) Side table.** The framework writes a `key → GlobalOpRef` table beside the store. Simple, but
  it is a second artefact that can drift from the operations — exactly the smell R11/R13 removed for
  `Preflet` and the plaintext tables.
- **(b) An attribute rank, `d.extkey`.** The external key is minted as an ordinary content cell on a
  `d.extkey` rank off each projected cell — identical to how `d.role`/`d.mime`/`d.media` carry
  attributes ([`zz_xudu_projector.cpp:37`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)). The
  key then travels in hypertime, is scrubbable, is published in a seal like any other operation, and
  is read back by a walk of `d.extkey` — no side artefact. **(b) is the recommendation**, because it
  reuses the engine's own answer for metadata (R13) and keeps the mapping inside the one address
  space the convergence insists everything live in.

### 5.4 Why the map is the lens's, not the engine's

The engine must not learn about inodes, paths, or any source's identity scheme — that is precisely
the coupling the lens exists to remove. The `ProjectionSession` holds the *table* (two
`unordered_map`s) but treats keys as opaque strings; only the lens computes a key (`stat` for the
filesystem, the `(documentId, version, span)` triple for xanadocs) and only the lens re-derives one
after an operation. This keeps `d.extkey` and the inode scheme entirely inside `FilesystemLens`, and
keeps `apps/common/xanadu/` free of any `<sys/stat.h>` dependency except in the one new lens file.

______________________________________________________________________

## 6. Round-Trip Fidelity and the Dirty-Cell Diff

### 6.1 Computing the diff against the COW base

The arena is a copy-on-write overlay of a base `Manifold`
([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp), the "overlay" doc comment): a
read of an unmodified cell falls through to the base, and the first *write* to one **shadows** it. A
cell the session has written is therefore exactly a cell the arena *holds its own slot for*, which
`ArenaManifold::holdsOwn()` already answers
([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp), `holdsOwn` doc comment: "a
cell this arena minted or has shadowed, as opposed to one it is merely reading through"). So the
dirty set is free — it is the set of cells for which `holdsOwn()` is true and whose slot differs
from the base's.

`commit()` therefore:

1. For each `CellRef` the arena holds its own slot for, compare against the base slot (content run,
   value bits, each `DimLink`). A cell present in the arena but not the base is a **create**;
   present in the base but shadowed to empty/unlinked is a **delete**.
1. Classify the delta: content changed → `applyContentChange`; an attribute-rank neighbour changed →
   `applyAttributeChange`; a `d.contains` / `d.seq` neighbour changed → `applyReparent`; new cell →
   `applyCreate`; removed → `applyDelete`.
1. Translate the `CellRef` to its `ExternalKey` via `cellToKey_`, order the operations (creates →
   relinks → deletes), call the lens, and repair the map from each returned key.

When there is no base (a fresh projection the user then edits in place), the session snapshots the
arena immediately after projection and diffs against the snapshot — the same algorithm with the
snapshot standing in for the base.

Because navigation never writes a slot (R8, invariant 7 — a cursor move is a relink of an
*ephemeral* cursor cell, not of a projected cell), a browse-only session produces an empty dirty set
and `commit()` is a no-op. This is the mechanism that makes "view movement is not an edit" fall out
rather than needing a rule at each call site.

### 6.2 Fidelity table

| aspect                          | project (source → cells) | unproject (cells → source)     | faithful?                                         |
| ------------------------------- | ------------------------ | ------------------------------ | ------------------------------------------------- |
| xanadoc paragraph in one piece  | one cell, one span       | content restated               | faithful                                          |
| xanadoc paragraph across pieces | first part only          | first part only                | **lossy** (one span per cell; §2.3)               |
| xanadoc cell id                 | → ops index              | ops index as DTO id            | **lossy on id**, faithful on structure            |
| xanadoc presentation metadata   | DTO carries it           | not reconstructed              | **lossy** (`storeToSlice` emits none)             |
| fs file name                    | `d.name` / content cell  | `rename(2)`                    | faithful (inode key preserved)                    |
| fs file content                 | addressed span / scalar  | write via `applyContentChange` | faithful if lens re-reads/writes bytes            |
| fs size / mtime                 | scalar cell on rank      | `utime(2)` / derived           | mtime faithful; size is **derived**, not settable |
| fs permissions                  | scalar cell `d.perm`     | `chmod(2)`                     | faithful                                          |
| fs move between dirs            | `d.contains` edge        | `rename(2)` across parents     | faithful (inode key preserved)                    |
| fs cross-device move            | `d.contains` edge        | copy+delete, key rebound       | faithful on identity, **new inode**               |
| fs on no-inode mount            | path-hash key            | rename = delete+create         | **lossy on identity** (§5.2)                      |
| fs symlink target               | `d.ref` edge             | `symlink(2)`                   | faithful                                          |
| fs hardlink (2 names)           | one cell, two `d.name`   | two paths, one inode           | faithful                                          |

**Faithful** means a project-then-unproject with no edit changes the source not at all, and an edit
unprojects to exactly the operation the user intended. **Lossy** entries are the honest limits: the
single-span-per-cell truncation is the convergence's own U3 limit
([`store-slice-convergence.md`](store-slice-convergence.md) U3), the presentation-metadata loss is
`storeToSlice`'s (§2.3), and the no-inode identity loss is the price of a filesystem that does not
expose stable identity.

### 6.3 What size being derived means

A file's size is a *consequence* of its content, not an independently settable attribute. The lens
projects it as a scalar cell for display and query, but `applyAttributeChange("d.size", …)` must
**refuse** (`std::unexpected`) rather than pretend — truncating or extending a file by setting a
number is not what the gesture means. This is the same discipline as the engine refusing a
signalling NaN at the API boundary ([`store.hpp`](apps/common/xanadu/store.hpp), `makeScalarCell`
`@throws`): a derived attribute is read-only, and saying so loudly beats a silent wrong write.

______________________________________________________________________

## 7. The Filesystem Lens, Worked

`FilesystemLens` projects a directory tree rooted at a path. It is the second implementation and the
one that proves the trait is not xanadoc-shaped.

### 7.1 Projection

```cpp
// apps/common/xanadu/zigzag/filesystem_lens.hpp  (proposed, the one new source file)
class FilesystemLens final : public ProjectionLens {
public:
  explicit FilesystemLens(std::filesystem::path root);

  std::vector<std::pair<std::string, std::optional<GenericDim>>> dimensions() const override {
    return {
      {"d.contains", GenericDim::Containment},
      {"d.seq",      GenericDim::Sequence},
      {"d.ref",      GenericDim::Reference},
      {"d.role",     std::nullopt},   // "file" | "dir" | "symlink"
      {"d.mime",     std::nullopt},
      {"d.size",     std::nullopt},
      {"d.mtime",    std::nullopt},
      {"d.perm",     std::nullopt},
      {"d.name",     std::nullopt},
      {"d.extkey",   std::nullopt},   // §5.3 (b): the stable key, in hypertime
    };
  }

  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)>) const override;
  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)>) const override;
  // apply*() lower to rename(2)/chmod(2)/utime(2)/mkdir(2)/unlink(2)
};
```

Walking the tree in a deterministic order (a sorted depth-first walk — invariant 6):

- Each entry becomes a `SourceNode` whose `key` is `"fs:<st_dev>:<st_ino>"` (§5.2), whose `content`
  is the file name (or the file's bytes, addressed as a span once imported into the permascroll via
  `importFileStore`, [`store_loader.hpp:45`](apps/common/xanadu/store_loader.hpp), for a persistent
  projection), and whose `attributes` carry `d.role`, `d.mime` (libmagic, as the engine already
  resolves MIME, [`zzstructure.hpp`](apps/common/xanadu/zigzag/zzstructure.hpp), `Cell::mime_type`
  comment), `d.size`, `d.mtime`, `d.perm` as scalar renderings.
- `forEachEdge` emits a `d.contains` edge from each directory to each child (POS), a `d.seq` edge
  chaining siblings in sorted order, and a `d.ref` edge from each symlink to its target's key.

The `ProjectionSession` mints a cell per node — size, mtime, and perm as scalar cells
(`makeScalarCell`, [`store.hpp:496`](apps/common/xanadu/store.hpp), so a VQL query can compare them
without parsing) — records `key → CellRef`, then minted the edges by translating both ends through
the map. Nothing on disk is touched; the arena is a live view of the directory.

### 7.2 Unprojecting edits

- **Rename a file in the visualizer** → the cell's `d.name` content changes → `commit()` classifies
  a content change on a `d.name` cell → `applyContentChange(key, "newname")` → the lens calls
  `std::filesystem::rename(oldpath, newpath)` and returns the *same* `ExternalKey` (inode
  unchanged), so the map needs no repair.
- **Drag a file between directories** → the cell's `d.contains` negward neighbour changes →
  `applyReparent(key, oldDirKey, newDirKey, order)` → `rename(2)` across parents, same inode, same
  key.
- **Change permissions** (e.g. edit the `d.perm` scalar) →
  `applyAttributeChange(key, "d.perm", mode)` → `chmod(2)`.
- **Change mtime** → `applyAttributeChange(key, "d.mtime", iso8601)` → `utime(2)`.
- **Try to change size** → `applyAttributeChange(key, "d.size", …)` →
  `std::unexpected("size is derived")` (§6.3).
- **Create a cell under a directory** → `applyCreate(node, parentKey)` → `mkdir(2)` or `open(2)` +
  write, returning the new inode's key.
- **Delete a cell** → `applyDelete(key)` → `unlink(2)` / `rmdir(2)`.

### 7.3 Persisting a directory as a slice

`ProjectionSession::promoteInto(store, parent, root)` turns the browsed directory into a durable
xanadoc slice: file bytes are imported into the permascroll
([`store_loader.hpp:45`](apps/common/xanadu/store_loader.hpp)), scratch renderings of scalars
acquire addresses, and the arena cells become `CompactOpNode`s — the single `promote()` crossing
(invariant 5). The `d.extkey` cells travel with them (§5.3 (b)), so re-opening the slice and
re-projecting the live directory can reconcile the two by key. This is "intertwingularity" from
[`vlace-functional-language.md` §13.4 example 7](vlace-functional-language.md): one value that is
both a directory and an addressable ZigZag structure.

______________________________________________________________________

## 8. Genuine New C++ vs Reused Engine Primitives

### 8.1 The genuinely new code (small)

1. **`projection_lens.hpp`** — the `ProjectionLens` trait, `SourceNode`, `SourceEdge`,
   `ExternalKey`, `GenericDim`. Pure interface, no logic.
1. **`projection_session.{hpp,cpp}`** — the framework: the project loop (mint dimensions → nodes →
   edges), the identity map (two `unordered_map`s), the dirty-cell diff against the COW base (§6.1),
   and the create/relink/delete ordering in `commit()`. This is the one piece with real logic, and
   it is **source-agnostic** — written once, works for every lens.
1. **`filesystem_lens.{hpp,cpp}`** — the only source-specific new file: the `stat`-based key scheme,
   the sorted tree walk, and the `apply*()` lowerings to `rename`/`chmod`/`utime`/`mkdir`/`unlink`.
   This is the only file that includes `<sys/stat.h>`.
1. **`XanadocLens`** — a thin re-expression of the existing projector behind the trait (§9), adding
   no behaviour.

### 8.2 Reused engine primitives (everything load-bearing)

- **Cell minting, linking, scalars, dimensions, splicing** — the entire Structure API already
  exists: `sliceGenesis` ([`store.hpp:446`](apps/common/xanadu/store.hpp)), `makeCell` /
  `makeScalarCell` ([`store.hpp:496`](apps/common/xanadu/store.hpp)), `setLink`, `spliceCell` /
  `spliceCellSpan` ([`store.hpp:588`](apps/common/xanadu/store.hpp)), `makeDimension`
  ([`store.hpp`](apps/common/xanadu/store.hpp)).
- **The arena write path and COW overlay** — `makeCell` / `makeScalarCell` / `link` / `setContent` /
  `ensureDimension` / `intern` and `holdsOwn()` for the dirty set
  ([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp)).
- **`promote()`** — the single persistence crossing
  ([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp), `promote` free function).
- **The identity-and-boundary machinery** — `ephemeralBit`, `isEphemeral`, `applyStructure`'s
  `EphemeralTarget` refusal, `Store::setLink`'s throw
  ([`manifold.hpp`](apps/common/xanadu/zigzag/manifold.hpp),
  [`store.hpp`](apps/common/xanadu/store.hpp)).
- **Attribute-rank metadata** — `d.role` / `d.mime` / `d.media` and the attach/read-back pattern
  ([`zz_xudu_projector.cpp:37`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)), generalized to
  `d.size` / `d.mtime` / `d.perm` / `d.extkey`.
- **Dimension lookup/creation** — `DimensionRegistry::getOrCreate`
  ([`dimension_registry.hpp:115`](apps/common/xanadu/zigzag/dimension_registry.hpp)),
  `Manifold::dimensionNamed` ([`manifold.hpp`](apps/common/xanadu/zigzag/manifold.hpp)).
- **File import** — `importFileStore` ([`store_loader.hpp:45`](apps/common/xanadu/store_loader.hpp))
  for turning file bytes into permascroll spans.
- **Multi-store composition** — `ArenaManifold::attach(Space)` for hosting a xanadoc lens and a
  filesystem lens in one arena
  ([`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp)).

The ratio is the point: **three new files, one with real logic, over a complete set of existing
primitives.** The lens is a refactor plus one new source, not new engine machinery.

______________________________________________________________________

## 9. Mapping the Existing Projector onto the Lens with Zero Behaviour Change

`XanadocLens` wraps the existing functions rather than reimplementing them, so the characterization
tests (`tests/zigzag/test_xudu_convergence.cpp`) and `xudu-dump --section=ops` stay byte-identical:

- `dimensions()` returns `d.doc`→`Sequence`, `d.transclude`→(custom), `d.link`→`Reference`,
  `d.version`→(custom), `d.clone`→`Clone`, plus `d.role`/`d.mime`/`d.media` — exactly the
  `XuduProjectorOptions` set
  ([`zz_xudu_projector.hpp`](apps/common/xanadu/zigzag/zz_xudu_projector.hpp)).
- `forEachNode` / `forEachEdge` are driven by the *same* paragraph-splitting and span-overlap logic
  `projectXuduToZigzag` already runs
  ([`zz_xudu_projector.cpp:43`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)); the lens simply
  surfaces the cells and edges it already computes.
- The `ExternalKey` for a xanadoc cell is the provenance triple the projector already tracks,
  `XuduProjectionProvenance`
  ([`zz_xudu_projector.hpp`](apps/common/xanadu/zigzag/zz_xudu_projector.hpp)):
  `"xd:<documentId>:<version>:<span>"`. The identity map is then the generalization of
  `ProjectedXuduStore::sourceByProjectedCell`
  ([`zz_xudu_projector.hpp`](apps/common/xanadu/zigzag/zz_xudu_projector.hpp)).
- The persistent path is still `sliceToStore()`
  ([`zz_xudu_projector.cpp:548`](apps/common/xanadu/zigzag/zz_xudu_projector.cpp)); `XanadocLens` is
  the arena-side front, `sliceToStore()` the promote-equivalent, so the Structure operations minted
  are the same ones in the same order (invariant 6).

Because `XanadocLens` *calls* the existing code, "zero behaviour change" is verifiable by diffing
`xudu-dump --section=ops` over every checked-in fixture before and after the refactor — the
convergence's own regression method ([`store-slice-convergence.md`](store-slice-convergence.md) §9,
"diff meaning, not bytes").

______________________________________________________________________

## 10. Phased Implementation Plan

Each phase is one commit; after each, `make -j$(nproc)` builds and `make -j$(nproc) test` passes.

1. **The trait and the framework, no lens.** Add `projection_lens.hpp` and
   `projection_session.{hpp,cpp}` with the project loop, the identity map, and `promoteInto()`.
   Unit- test with a trivial in-memory lens (three nodes, two edges) asserting the identity map and
   a tight arena. No source-specific code yet.
1. **`XanadocLens` over the existing projector.** Re-express the xanadoc path behind the trait,
   calling `projectXuduToZigzag` / `sliceToStore` unchanged. Gate: `xudu-dump --section=ops` on
   every fixture is byte-identical, and `tests/zigzag/test_xudu_convergence.cpp` passes unmodified.
1. **The dirty-cell diff and `commit()`.** Implement the COW-base diff via `holdsOwn()` and the
   classify/order/apply loop. Test with `XanadocLens`: a cell relinked in the arena unprojects to a
   `Store::setLink`, a content splice to `Store::spliceCell`, and a browse-only session produces an
   empty dirty set (R8).
1. **`FilesystemLens` projection (read-only).** The `stat`-based key, the sorted tree walk, the
   attribute ranks. Test over a fixture directory: a known tree projects to a known cell count and
   rank layout, deterministically (same tree → byte-identical arena).
1. **`FilesystemLens` unprojection.** The `apply*()` lowerings against a temp directory: rename,
   move, chmod, utime, create, delete, and the refusal of a size change. Assert the inode key
   survives a rename and is rebound on a cross-device move (simulated).
1. **Persistent projection and `d.extkey`.** `promoteInto()` with file bytes imported via
   `importFileStore`, scalars rendered, `d.extkey` minted; re-open and reconcile by key.
1. **Mixed-source session.** One `ProjectionSession` hosting a `XanadocLens` and a `FilesystemLens`
   via `ArenaManifold::attach`, proving the string-keyed map keeps the two identity spaces disjoint.

______________________________________________________________________

## 11. Open Questions

- **U-L1. No-inode identity.** On a mount without stable inodes the path-hash key makes a rename a
  delete+create (§5.2). A gesture that carried rename *intent* could be lowered faithfully; a diff
  that only sees "cell A gone, cell B appeared" cannot distinguish rename from delete+create.
  Whether the visualizer should carry gesture intent into `commit()` is unresolved.
- **U-L2. Concurrent source mutation.** The arena is a COW snapshot; the real directory can change
  underneath it (another process writes a file). `commit()` should detect a key whose `stat` no
  longer matches the projected attributes and report a conflict rather than clobber — the design
  records the need, not the policy.
- **U-L3. Where the lens code lives.** `apps/common/xanadu/zigzag/` needs no graphics device and is
  the right home for the trait, the session, and `XanadocLens`. `FilesystemLens` touches
  `<sys/stat.h>` only; it belongs there too (it draws nothing), but its platform specifics
  (`st_dev`/`st_ino` vs a Windows file id) want an abstraction the design leaves to implementation.
