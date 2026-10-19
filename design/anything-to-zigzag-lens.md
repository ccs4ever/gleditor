# The Anything-to-ZigZag Lens: Generalizing the Xanadu Projector into a Bidirectional Source Lens

A design specification for generalizing the Xanadu-to-ZigZag translator
([`xanadu_projector.{hpp,cpp}`](apps/common/xanadu/zigzag/xanadu_projector.hpp)) into a
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
  ([`xanadu_projector.hpp:92`](apps/common/xanadu/zigzag/xanadu_projector.hpp));
- its *dimensions* are the xanadoc-specific `d.doc` / `d.transclude` / `d.link` / `d.version` /
  `d.clone` set, baked into `XuduProjectorOptions`
  ([`xanadu_projector.hpp`](apps/common/xanadu/zigzag/xanadu_projector.hpp), the
  `XuduProjectorOptions` struct);
- its *projection logic* is paragraph-splitting and span-overlap heuristics specific to concatext
  (the assembled, transclusion-resolved document text)
  ([`xanadu_projector.cpp:43`](apps/common/xanadu/zigzag/xanadu_projector.cpp),
  `projectXanaduToZigzag`).

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

1. the existing xanadoc projector is **re-expressed as `XanadocLens` on the lens primitives** — not
   necessarily byte-for-byte identical to today's output, because nothing is in production and the
   convergence lets a shape change (bump the version, write the new shape, delete the old reader).
   The bar is *semantic equivalence or improvement*: every checked-in fixture still round-trips
   through `project → unproject` with the same meaning, verified by `xudu-dump --section=ops` diffed
   for meaning rather than bytes ([`store-slice-convergence.md`](store-slice-convergence.md) §9,
   "diff meaning, not bytes"), and the lens model is expected to *fix* several of today's losses
   (§9), not preserve them;
1. a new `FilesystemLens` projects a directory tree with metadata and unprojects edits to real
   filesystem operations;
1. the hard problem — a filesystem has no append-only ops index, so a `CellRef` cannot be its stable
   identity the way R7 makes an ops index the xanadoc cell's identity — is solved by an explicit,
   lens-owned **external-key ↔ CellRef** mapping rather than by pretending the two spaces coincide.

______________________________________________________________________

## 2. Evidence: How the Current Projector Works

Read in full: `xanadu_projector.{hpp,cpp}`, `manifold.hpp`, `arena_manifold.hpp`, `zzstructure.hpp`,
the Structure API in `store.hpp`, and `store-slice-convergence.md` §3–§5, R7, R8.

### 2.1 Two translators, two source shapes, two targets

There are, confusingly, **two** translation surfaces in the one file, and the lens must subsume
both:

- **The DTO projector.** `projectXanaduToZigzag()`
  ([`xanadu_projector.cpp:43`](apps/common/xanadu/zigzag/xanadu_projector.cpp)) takes a
  `std::vector<XanaduDocInput>` and `std::vector<xanadu::Link>` and produces a `ZzStructureDocument`
  — the in-memory DTO ([`zzstructure.hpp`](apps/common/xanadu/zigzag/zzstructure.hpp),
  `ZzStructureDocument`). It reads assembled *text* and *spans*, not a `Store`.
  `projectStoreToZigzag` / `projectStoreWithProvenance`
  ([`xanadu_projector.cpp:343`](apps/common/xanadu/zigzag/xanadu_projector.cpp)) are thin wrappers
  that first `store.rebuild(verId)` each version, assemble its text through
  `store.read`/`store.resolve`, and hand the result to the DTO projector. The target here is the
  DTO, not a `Manifold`.
- **The convergence translators.** `sliceToStore()`
  ([`xanadu_projector.cpp:548`](apps/common/xanadu/zigzag/xanadu_projector.cpp)) mints a
  `ZzStructureDocument` into a `xanadu::Store` **as `OpKind::Structure` operations** via the
  Structure API, and `storeToSlice()`
  ([`xanadu_projector.cpp:678`](apps/common/xanadu/zigzag/xanadu_projector.cpp)) folds a `Manifold`
  back into a DTO. These are the real cell-minting path; the DTO projector is the legacy
  paragraph-splitting path that step 20 of the convergence is retiring.

### 2.2 How documents/branches/links become cells on dimensions

In the DTO projector:

- **`d.doc` — reading flow.** Each paragraph (or whole document) becomes a `Cell`; sequential cells
  within one document are linked `pos`/`neg` on `opts.doc_dimension`
  ([`xanadu_projector.cpp`](apps/common/xanadu/zigzag/xanadu_projector.cpp), the `docCellChains`
  loop under comment `--- 1. Link sequential reading order`).
- **`d.transclude` — shared spans.** Cells in *different* documents whose spans overlap
  (`spansOverlap`, [`xanadu_projector.cpp:28`](apps/common/xanadu/zigzag/xanadu_projector.cpp)) are
  linked on `opts.transclusion_dimension` (`--- 2. Link transclusions`).
- **`d.version` — microversion lineage.** Positionally-aligned cells across consecutive documents
  are linked on `opts.version_dimension` (`--- 3. Link sequential microversion steps`).
- **`d.link` — xanalinks.** Each `xanadu::Link`'s left/right endsets are matched to cells by span
  overlap and linked on `opts.link_dimension` (`--- 3. Link Xanalinks`).
- **`d.clone` — unchanged clones.** A paragraph whose span was already seen is minted as a
  content-free clone (`cell.data = ""`, `cell.role = "xudu_clone"`) linked on `opts.clone_dimension`
  to its predecessor (the `isUnchangedClone` branch).

In the convergence translator `sliceToStore()`, `role` / `mime_type` / `media_path` do **not** ride
in `CompactOpNode` fields — they become **cells on their own ranks** `d.role` / `d.mime` / `d.media`
([`xanadu_projector.cpp:37`](apps/common/xanadu/zigzag/xanadu_projector.cpp)), attached posward of
the cell they describe (the `attach` lambda in `sliceToStore`). `storeToSlice()` reads them back off
those ranks and classes the attribute cells and the dimension cells as *structure* (the `structural`
set in `storeToSlice`,
[`xanadu_projector.cpp:700`-ish](apps/common/xanadu/zigzag/xanadu_projector.cpp)), so a round trip
does not re-emit them as content. This is R13's "metadata is cells on a dimension" and is exactly
the mechanism a filesystem lens needs for file metadata.

### 2.3 Which direction is lossy

Both directions lose something, and the losses are different:

- **Project (source → cells), lossy.** A paragraph straddling two pieces is addressed by its first
  part only, because a cell holds one span — the code says so in a comment: "Clamped to the piece
  the paragraph starts in, because a cell holds one span … a truncation, not a misdirection"
  ([`xanadu_projector.cpp`](apps/common/xanadu/zigzag/xanadu_projector.cpp), inside the
  `split_by_paragraphs` branch). `storeToLinkPackage()` publishes a multi-span cell "under the
  address of its first" ([`xanadu_projector.cpp`](apps/common/xanadu/zigzag/xanadu_projector.cpp),
  the `contentRun.front()` comment).
- **Unproject (cells → DTO), lossy on ids.** `storeToSlice()`'s own doc comment: "a CellRef is an
  operation index and is used directly as the YAML id, so a round trip renumbers cells rather than
  restoring whatever numbers the original file used"
  ([`xanadu_projector.hpp`](apps/common/xanadu/zigzag/xanadu_projector.hpp), `storeToSlice` doc
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
[`xanadu_projector.cpp`](apps/common/xanadu/zigzag/xanadu_projector.cpp)), so the DTO's own ids do
not survive — the cell's identity becomes its name in hypertime (R4, R7). This is the single most
important fact for the generalized lens: **the engine side owns identity as an ops index; the source
side's own identity is a separate space the lens must map.**

These losses (first-span truncation, id renumbering, dropped presentation metadata) describe
*today's* projector, not a constraint the lens inherits. Nothing is in production, so `XanadocLens`
is free to improve on them rather than reproduce them; §9.2 says which it fixes and how, and the
regression bar becomes meaning-equivalence rather than byte-equality (§9.3).

### 2.5 Where projection reads and where it writes

- **DTO projector reads** assembled text + spans from `Store::rebuild` / `read` / `resolve`
  ([`xanadu_projector.cpp:343`](apps/common/xanadu/zigzag/xanadu_projector.cpp)) and **writes** a
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
   ([`xanadu_projector.cpp`](apps/common/xanadu/zigzag/xanadu_projector.cpp), the "Posward links
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
   ([`xanadu_projector.cpp`](apps/common/xanadu/zigzag/xanadu_projector.cpp), the
   `std::ranges::sort` on `ids`;
   [`xanadu_projector.hpp`](apps/common/xanadu/zigzag/xanadu_projector.hpp), `sliceToStore` doc
   comment). A lens must impose a total order on source nodes so a fixture can be regenerated and
   diffed.
1. **R8 — view movement is not an edit.** Navigating the projected manifold mints nothing in the
   source. Only a *user edit* to the structure (rename, move, metadata change) unprojects to a
   source operation ([`store-slice-convergence.md`](store-slice-convergence.md) R8).

______________________________________________________________________

## 4. The Proposed Interface: `ProjectionLens`

The lens is a trait — here, a C++ abstract base class with two virtual halves, not a
`std::is_*`-style type trait. The **project** half turns a source into arena cells; the
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
/// For a xanadoc this encodes the (documentId, version, span) triple as
/// "xd:<documentId>:<version>:<span>"; for a filesystem it is the inode-or-path
/// key of §5 ("fs:<dev>:<ino>"). The lens is the only code that knows how to
/// compute, compare, and persist one.
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
  /// pre-mint pass (xanadu_projector.cpp, the `dimensionNames` set).
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

A query engine (VQL) and a logic runtime (Vlog) resolve a dimension by its **literal rank-name
string** — there is no indirection layer that would let an enum tag stand in for a name
(`MultiStoreCoordinator::resolveDimension` matches a fixed privileged set and otherwise calls
`findOrMintDimension(name)`, [`multi_store.cpp:329`](apps/common/xanadu/multi_store.cpp)). So for
"the same query traverses any lens" to be true, the generic names below are **the actual rank
strings every lens mints**, not an enum decoration beside source-specific names. `GenericDim` is the
C++-side token a lens maps a dimension onto; the string in the second column is what lands in the
manifold and what a query names. A lens that minted its own private names instead (the old
`d.doc`/`d.link`) would make a generic query silently mint a fresh empty dimension and return
nothing — the single most dangerous failure mode, because it is not an error (§4.3.1).

"Generic" means one of two things, and the table's column covers both: a **`GenericDim` structural
dimension** every lens maps onto (`d.contains`/`d.seq`/`d.ref`/`d.clone`), or a **conventionally
named attribute rank** the framework recognizes (`d.role`/`d.mime`) that is not in the enum but is
pinned by name across lenses.

| dimension      | kind           | xanadoc meaning            | filesystem meaning              |
| -------------- | -------------- | -------------------------- | ------------------------------- |
| `d.contains`   | generic (enum) | document → paragraph       | directory → entry               |
| `d.seq`        | generic (enum) | reading order              | sibling order within a dir      |
| `d.ref`        | generic (enum) | xanalink                   | symlink / hardlink target       |
| `d.clone`      | generic (enum) | shared-span clone          | identical-content files (dedup) |
| `d.role`       | generic (attr) | `role` attribute cell      | `file`/`dir`/`symlink` kind     |
| `d.mime`       | generic (attr) | MIME attribute cell        | libmagic MIME of file           |
| `d.transclude` | xanadoc custom | overlapping primedia spans | — (no analogue)                 |
| `d.version`    | xanadoc custom | microversion lineage       | — (no analogue)                 |
| `d.size`       | fs custom      | —                          | file size in bytes (scalar)     |
| `d.mtime`      | fs custom      | —                          | mtime (scalar / ISO-8601 cell)  |
| `d.perm`       | fs custom      | —                          | mode bits (scalar)              |

The four `GenericDim` values plus the two pinned attribute ranks are the vocabulary a visualizer and
a query rely on across every lens; `d.role` / `d.mime` are the attribute ranks already named in the
engine ([`xanadu_projector.cpp:37`](apps/common/xanadu/zigzag/xanadu_projector.cpp)), populated
through `SourceNode::attributes`. A source-specific concept with no generic analogue
(`d.transclude`, `d.size`) is declared per lens via `dimensions()` under its own name and is simply
not reachable by a generic cross-lens query — which is honest, not a failure.

#### 4.3.1 Dimension-name resolution is the load-bearing contract

Because VQL/Vlog bind dimensions by string, the generic-name rule is not a stylistic preference — it
is the contract that makes the whole portability claim work, and it has two possible shapes:

- **(a) Lenses mint the canonical generic strings (recommended).** `XanadocLens` mints `d.contains`
  / `d.seq` / `d.ref` directly, not `d.doc` / `d.link`. Nothing is in production, so the xanadoc
  projection is free to use the generic names (the convergence permits a shape change). A query over
  any lens is then identical, with no engine change.
- **(b) The engine gains an alias layer.** `resolveDimension` learns to map a canonical generic name
  onto a lens's private name. This is more machinery, touches the query hot path, and must be
  per-session (two lenses disagree on what `d.ref` aliases), so (a) is preferred. The design assumes
  **(a)** everywhere below; §9 revises `XanadocLens` to mint the generic names, and the few
  genuinely xanadoc-only ranks (`d.transclude`, `d.version`) keep their own names because no generic
  analogue exists.

The pinned attribute names (`d.role`, `d.mime`) must additionally be in the query engine's
privileged set so a predicate like `[/d.mime = 'image/png']` resolves identically across sources;
today only `d.role` and `d.clone` are privileged
([`multi_store.cpp:333`](apps/common/xanadu/multi_store.cpp)), so adding `d.mime` (and the generic
structural names) to that set is a prerequisite the plan carries (§10).

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
  /// source: the single promote() crossing (invariant 5). Takes a SET of roots,
  /// not one: a directory tree has several top-level entries and a mixed-source
  /// arena has disjoint identity islands, neither of which is single-rooted the
  /// way promote(root) assumes. The session either synthesizes a genesis home
  /// linking all roots under d.contains, or loops promote() threading the
  /// version forward and reconciling the ref maps. It MUST deduplicate d.clone
  /// members (see note below) so a shared subgraph costs O(log n) ops, not O(n).
  [[nodiscard]] std::optional<Promoted>
  promoteInto(xanadu::Store &store, const MicroversionId &parent,
              std::span<const CellRef> roots);

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

### 5.2 The scheme: `(device, inode)` with a path fallback

For a filesystem that exposes inodes (Linux/macOS/BSD), the stable key is the pair `st_dev` ×
`st_ino` from `stat(2)`, which a rename and a move-within-a-filesystem preserve:

```
ExternalKey (filesystem) ::= "fs:" <st_dev> ":" <st_ino>              -- inode-stable
                           | "fsp:" <sha256(canonical_path)>          -- fallback, no inode
```

(Here *canonical_path* is `std::filesystem::weakly_canonical` of the entry's path — a stable,
symlink-resolved, lexically-normalized spelling.)

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
`attach(Space)`): a xanadoc key (`"xd:<documentId>:<version>:<span>"`) and a filesystem key
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
  attributes ([`xanadu_projector.cpp:37`](apps/common/xanadu/zigzag/xanadu_projector.cpp)). The key
  then travels in hypertime, is scrubbable, is published in a seal like any other operation, and is
  read back by a walk of `d.extkey` — no side artefact. **(b) is the recommendation**, because it
  reuses the engine's own answer for metadata (R13) and keeps the mapping inside the one address
  space the convergence insists everything live in.

  **Caveat (architect finding).** A `d.extkey` cell is a reachable neighbour, so `promote()` walks
  and persists it — which means the key becomes an authored Structure op that enlarges a fixture's
  op sequence and makes a *stale* key an authored operation rather than a rewritable side-table
  entry. Two honest resolutions, decided per deployment: either `promote()` **skips** the `d.extkey`
  rank the way it already skips `d.dims` (keeping the key out of the content hash and the publish
  seal), or the key stays a framework **side table** (option (a)) for the publish path and only the
  live-session map is in-arena. The doc's recommendation is (b)-with-skip; what it must stop
  claiming is that the hypertime-cell option is cost-free (U-L5).

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

| aspect                          | project (source → cells)                     | unproject (cells → source)     | faithful?                                         |
| ------------------------------- | -------------------------------------------- | ------------------------------ | ------------------------------------------------- |
| xanadoc paragraph in one piece  | one cell, one span                           | content restated               | faithful                                          |
| xanadoc paragraph across pieces | legacy: first part only; lens: full span run | full span run                  | **fixed by `XanadocLens`** (§9.2; Splice run)     |
| xanadoc cell id                 | ops index (legacy) / `d.extkey` (lens)       | stable key under lens          | **fixed by `XanadocLens`** (§9.2)                 |
| xanadoc presentation metadata   | legacy drops it; lens: attribute ranks       | round-tripped on ranks         | **fixed by `XanadocLens`** (§9.2, R13)            |
| fs file name                    | `d.name` / content cell                      | `rename(2)`                    | faithful (inode key preserved)                    |
| fs file content                 | addressed span / scalar                      | write via `applyContentChange` | faithful if lens re-reads/writes bytes            |
| fs size / mtime                 | scalar cell on rank                          | `utime(2)` / derived           | mtime faithful; size is **derived**, not settable |
| fs permissions                  | scalar cell `d.perm`                         | `chmod(2)`                     | faithful                                          |
| fs move between dirs            | `d.contains` edge                            | `rename(2)` across parents     | faithful (inode key preserved)                    |
| fs cross-device move            | `d.contains` edge                            | copy+delete, key rebound       | faithful on identity, **new inode**               |
| fs on no-inode mount            | path-hash key                                | rename = delete+create         | **lossy on identity** (§5.2)                      |
| fs symlink target               | `d.ref` edge                                 | `symlink(2)`                   | faithful                                          |
| fs hardlink (2 names)           | one cell, two `d.name`                       | two paths, one inode           | faithful                                          |

**Faithful** means a project-then-unproject with no edit changes the source not at all, and an edit
unprojects to exactly the operation the user intended. The three **fixed by `XanadocLens`** entries
are losses the *legacy* DTO projector carried as compromises and the lens removes (§9.2): the
single-span truncation goes away once a node carries its whole span run (a cell's content is already
a `Splice` run of spans, [`store.hpp:588`](apps/common/xanadu/store.hpp); the one-span-per-op limit
just means a multi-span cell promotes as a splice per span, not that the cell cannot hold them),
presentation metadata rides attribute ranks (R13), and cell id is stabilized by `d.extkey`. The
genuinely remaining limits are source-side, not projector-side: a file's **size is derived** (§6.3)
and a **no-inode mount** cannot offer stable identity (§5.2) — neither is something a better
projector can fix.

### 6.3 What size being derived means

A file's size is a *consequence* of its content, not an independently settable attribute. The lens
projects it as a scalar cell for display and query, but `applyAttributeChange("d.size", …)` must
**refuse** (`std::unexpected`) rather than pretend — truncating or extending a file by setting a
number is not what the gesture means. This is the same discipline as the engine refusing a
signalling NaN at the API boundary ([`store.hpp`](apps/common/xanadu/store.hpp), `makeScalarCell`
`@throws`): a derived attribute is read-only, and saying so loudly beats a silent wrong write.

### 6.4 The per-cell diff is not enough: rank-level diffing and `applyReorder`

A per-cell `holdsOwn()` comparison sees content, value bits, and each cell's `pos`/`neg` link along
every dimension — enough for a content edit, a scalar change, a cleared span, or a single relink. It
is **not** enough for the headline filesystem gesture, *reordering siblings within a rank*: moving
one child reshapes the `pos`/`neg` of several cells, so a naive diff emits N uncoalescable
`applyReparent` calls instead of one "the order is now X" operation, and a source whose order is a
single column (a directory listing, a junction table's sort key) cannot faithfully receive N
disconnected moves.

So `commit()` diffs at **two granularities**: per-cell for content/attribute/value changes, and
**per-rank** for structure. For each dimension a changed cell touches, it walks the rank in the base
and in the arena and computes a sequence edit (LCS or a simpler insert/delete/move script), emitting
one ordered operation per rank rather than per edge. The trait gains:

```cpp
/// The ordered children of `parent` along `dim` changed to `newOrder`
/// (a reorder, an insert, or a removal within one rank). The lens lowers it to
/// one source operation -- a directory re-sort, a reordered junction, a moved
/// paragraph -- rather than receiving it as scattered per-cell reparents.
[[nodiscard]] virtual std::expected<ExternalKey, std::string>
applyReorder(const ExternalKey &parent, std::string_view dim,
             std::span<const ExternalKey> newOrder) = 0;
```

`applyReparent` keeps its role for *cross-parent* moves (a child changes which container it is
under); `applyReorder` handles *within-rank* order. The two are distinguished by whether the cell's
containment parent changed, which the rank diff knows.

### 6.5 `commit()` ordering is a topological sort, with a cycle policy

"Creates → relinks → deletes" is too coarse: a child created under a just-created parent needs the
parent minted first, and a `d.ref` cycle (two new symlinks pointing at each other) has no valid
linear order. `commit()` therefore topologically sorts the create/reparent operations over the
dependency DAG (an edge "A must exist before B" from each node to its container and to any node it
is linked to at creation), applies in that order, and handles the two hard cases explicitly:

- **Just-created parent.** `applyCreate` for a container completes before any `applyReparent` /
  `applyReorder` that names it as parent — guaranteed by the topological order, since containment is
  a DAG edge.
- **Reference cycles.** A `d.ref` back-edge that would close a cycle is deferred to a second pass
  after all endpoints exist (legal for symlinks, which may point anywhere; **refused** for
  containment, which must stay acyclic — a directory cannot contain its ancestor). The cycle policy
  is per-dimension-kind: `Reference` cycles are allowed and two-passed, `Containment` cycles are a
  `std::unexpected`.

Deletes run last, and a delete of a container must account for the source's own cascade (a
`DROP TABLE`, an `rm -r`): see §8A (the schema/instance node-kind bullet), because a cascade deletes
cells the per-cell diff never marked dirty.

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

`ProjectionSession::promoteInto(store, parent, roots)` turns the browsed directory into a durable
xanadoc slice: file bytes are imported into the permascroll
([`store_loader.hpp:45`](apps/common/xanadu/store_loader.hpp)), scratch renderings of scalars
acquire addresses, and the arena cells become `CompactOpNode`s — the single `promote()` crossing
(invariant 5), over the *set* of top-level entries (§4.4). The `d.extkey` cells travel with them
(§5.3 (b), with the promote-skip caveat), so re-opening the slice and re-projecting the live
directory can reconcile the two by key. This approaches the "intertwingularity" of
[`vlace-functional-language.md` §13.4 example 7](vlace-functional-language.md) — one value that is
both a directory and an addressable ZigZag structure — but note (§7.4) that example over-assumes a
vlace surface the language does not yet have.

### 7.4 The vlace surface: a lens entry point vlace does not have yet

A vlace programmer who takes `vlace-functional-language.md` §13 literally ("compute over it in
vlace") hits a wall the lens design is silent on: today vlace's `std:xanadu` natives
(`makeText`/`makeDim`/`setLink`/`cellText`/`linked`/`promote`) can only build cells in the program's
*own* arena and read one neighbour at a time. There is no verb to **open a projected source** — a
program cannot obtain the root of a `FilesystemLens` projection at all, so the
word-frequency-over-a-directory task is a non-starter at step one. The design genuinely omits this
entry point. The smallest primitive that closes it:

```ocaml
openLens :: String -> IO Cell    -- project a source (by URI/path) via its lens,
                                  -- returning the arena root CellRef; an IO effect
                                  -- because it reads the external world.
```

`openLens "fs:/some/dir"` runs a `FilesystemLens` through a `ProjectionSession` *over the vlace
evaluator's own arena* (§9A.2's project-into-bound-arena contract, so the result is visible to
`linked`/`cellText`), and returns the root cell. With it, the histogram program is expressible using
the rank walk and `cellText` vlace already has. **This is the one lens-design gap for vlace;** it is
distinct from four *language* gaps that are not the lens's concern and are tracked against the vlace
std library, not here: a multi-step rank walk (`childrenOf : Cell -> Dim -> List Cell`, derivable
from `linked`), string tokenization (`words : String -> List String`), a `Map`/assoc type, and
string ordering for sorting (today vlace compares strings only with `==`/`/=`). The lens must
provide `openLens`; `std:fun` must grow the rest.

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
1. **`XanadocLens`** — the xanadoc source re-expressed on the lens primitives (§9). Free to drop
   today's paragraph-splitting heuristics and single-span truncation where the lens model does
   better; the only hard requirement is that a projected fixture still means the same thing after a
   round trip.

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
  **Caveat (architect finding):** today `promote()` follows `d.clone` edges like any other link and
  re-spools each reachable cell's content, so two clone members get *different* content addresses —
  it does **not** deduplicate shared subgraphs. The design's O(log n) transclusion-sharing claim is
  therefore a **requirement placed on promotion**, not a property it currently has: either the lens
  emits clone members as a master plus content-free members mapped to the master's address (as the
  xanadoc projector's empty-data clones already do,
  [`xanadu_projector.cpp`](apps/common/xanadu/zigzag/xanadu_projector.cpp), the `isUnchangedClone`
  branch), or `promote()` learns to resolve `cloneMaster()` and reuse spans. Flagged in §11 (U-L4).
- **The identity-and-boundary machinery** — `ephemeralBit`, `isEphemeral`, `applyStructure`'s
  `EphemeralTarget` refusal, `Store::setLink`'s throw
  ([`manifold.hpp`](apps/common/xanadu/zigzag/manifold.hpp),
  [`store.hpp`](apps/common/xanadu/store.hpp)).
- **Attribute-rank metadata** — `d.role` / `d.mime` / `d.media` and the attach/read-back pattern
  ([`xanadu_projector.cpp:37`](apps/common/xanadu/zigzag/xanadu_projector.cpp)), generalized to
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

## 8A. Extending the Trait for Structured Sources

A paper third lens — a live SQLite database (schema + rows) — stress-tested the trait against a
source that is neither a tree of text nor a flat file set. It projects (reads) cleanly, but it
exposed real gaps in the *unproject* (write) half, because the trait of §4.2 was shaped for
single-object, single-op sources (a file, a paragraph). None of these block the two worked lenses;
all of them are needed before the trait can safely *write back* to a structured store. They define
"v2" of the interface.

- **Edges must carry properties and order (HIGH).** `SourceEdge` is `{from, to, dimension, dir}`. A
  foreign key *is* a labelled, actioned relationship (`ON DELETE CASCADE`, a constraint name); an
  ordered many-to-many needs a per-edge sort key. Without a field for either, a lens must reify
  every non-trivial edge as a phantom node — the awkward encoding that signals a missing hook. Add
  `std::vector<std::pair<std::string,std::string>> properties;` and `std::uint64_t order{0};` to
  `SourceEdge`, and an `applyEdgeChange(from, to, dim, properties)` method so an edge's own
  attributes round-trip.
- **Node kind: schema vs instance (HIGH).** A row is *contained by* a table and *conforms to* its
  column schema — two relations the trait can only merge into one `d.contains`. Worse, deleting a
  schema node (`DROP TABLE`) must cascade to every instance under it, but the per-cell diff (§6.1)
  marks only the one shadowed cell dirty, so `commit()` under-reports the delete and the source's
  own cascade silently diverges from the manifold. Add a `NodeKind { Schema, Instance }` (or an open
  `std::string kind`) to `SourceNode`, and let the framework treat a declared `conformsTo` relation
  as cascade-significant when classifying deletes.
- **Content needs NULL and non-text/non-scalar (MEDIUM).** The `content` variant
  `{string,double,bool,span}` cannot express SQL `NULL` (≠ `''` ≠ `0`), forcing a parallel
  `d.nullflag` rank — the "meaning hidden beside the cell" anti-pattern. And a BLOB is none of the
  variant's types; importing a 2 MB blob via `importFileStore` just to *view* it mutates the
  author's permascroll for a read-only browse (violates R8's spirit). Add `std::monostate` (an
  explicit `Null`) and a lazy `BlobRef{bytes-provider}` alternative the framework projects
  by-reference without permascroll residency.
- **Transaction / bulk / atomicity hook (HIGH).** `commit()` applies one cell at a time and the
  per-change `ChangeReport` reports partial failure — the *wrong* contract for a transactional
  source, which must roll back a half-applied batch, not report "cell 7 of 12 failed." A row
  `INSERT` also needs all its value cells in one statement (a per-value apply risks a NOT NULL
  violation on the first partial write). Add `beginBatch()` / `endBatch(ok)` brackets `commit()`
  calls once around the whole dirty set, and a batched
  `applyCreate(std::span<const SourceNode>, parent)` for cohesive multi-node creations.
- **Multi-key rebind on cascade rename (MEDIUM).** A key that embeds a parent name
  (`sql:row:<table>:…`) is invalidated for *every descendant* by one `ALTER TABLE RENAME`, but an
  `apply*()` returns a single new key. Let the relevant methods return
  `std::vector<std::pair<ExternalKey,ExternalKey>>` (old→new rebindings) so the framework repairs
  the whole subtree.
- **`rekey()` on wholesale source re-indexing (MEDIUM).** `VACUUM` renumbers rowids out from under a
  PK-less table — the DB analogue of a device remount, but the *common* case, not an edge. Add an
  optional `rekey()` / `onSourceReindexed()` callback the framework invokes to rebuild the identity
  map wholesale instead of orphaning it.
- **`applyReparent` must receive the dimension (MEDIUM).** A "reparent" along `d.contains` (move a
  row to another table — no SQL verb) and along `d.ref` (re-point an FK — `UPDATE … SET fk=`) are
  structurally identical but semantically distinct; the method currently drops the dimension and so
  cannot tell them apart. Pass the dimension in.

The through-line: the *read* spine (`dimensions`/`forEachNode`/`forEachEdge` + attribute ranks +
string `ExternalKey` + the `holdsOwn` diff) genuinely generalizes; the *write* half needs these
seven additions before a relational or otherwise structured source is safe. A lens that only
projects (a read-only visualizer or query source) needs none of them, which is why the two worked
lenses and the VQL/vprolog surface (§9A) do not wait on this section.

______________________________________________________________________

## 9. Mapping the Existing Projector onto the Lens — and Improving It

Nothing is in production, so `XanadocLens` is not held to byte-for-byte parity with today's
`projectXanaduToZigzag` output. It is re-expressed on the lens primitives, and that re-expression is
the occasion to *fix* losses the current projector carries as deliberate compromises. The bar is
meaning: a fixture projected through `XanadocLens`, then unprojected, is the same document — not the
same byte sequence of operations.

### 9.1 What stays the same

- The genuinely xanadoc-only ranks keep their own names because no generic analogue exists:
  `d.transclude` (overlapping primedia spans) and `d.version` (microversion lineage), declared
  through `dimensions()` as custom rather than a per-call `XuduProjectorOptions` struct
  ([`xanadu_projector.hpp`](apps/common/xanadu/zigzag/xanadu_projector.hpp)).
- The `ExternalKey` for a xanadoc cell is the provenance triple the projector already tracks,
  `XanaduProjectionProvenance`
  ([`xanadu_projector.hpp`](apps/common/xanadu/zigzag/xanadu_projector.hpp)):
  `"xd:<documentId>:<version>:<span>"`. The identity map generalizes
  `ProjectedXuduStore::sourceByProjectedCell`
  ([`xanadu_projector.hpp`](apps/common/xanadu/zigzag/xanadu_projector.hpp)).
- Attribute metadata still rides on `d.role`/`d.mime`/`d.media` ranks
  ([`xanadu_projector.cpp:37`](apps/common/xanadu/zigzag/xanadu_projector.cpp)) — the mechanism the
  filesystem lens reuses for `d.size`/`d.mtime`/`d.perm`.

### 9.2 What the lens model lets us improve (and should)

The current projector's lossy entries (§2.3, §6.2) are not laws of the manifold; they are artefacts
of a projector written before the lens model existed. Under `XanadocLens` each becomes a design
choice to revisit:

- **Mint the canonical generic rank names, not `d.doc`/`d.link`.** This is the change that makes the
  portability promise real (§4.3.1): `XanadocLens` links reading flow on `d.seq`, document→paragraph
  on `d.contains`, and xanalinks on `d.ref` — the same strings `FilesystemLens` mints — so one VQL
  query or Prolog goal traverses either source (§9A). The old `d.doc`/`d.link` names were a pre-lens
  private vocabulary; retiring them is permitted (nothing is in production) and is a prerequisite
  for "traverse xanadocs as easily as slices." Only `d.transclude` and `d.version`, which have no
  generic analogue, keep xanadoc-specific names.

- **Multi-span cells instead of first-span truncation.** Today a paragraph straddling pieces is
  addressed by its first part only, "a truncation, not a misdirection"
  ([`xanadu_projector.cpp`](apps/common/xanadu/zigzag/xanadu_projector.cpp), the
  `split_by_paragraphs` branch) — the convergence's U3 single-span-per-cell limit. The lens's
  `SourceNode::content` is already a `std::span<const PrimediaSpan>`, so a node may carry *all* its
  spans. A cell's content is already modelled as a *run* of spans via `Splice` / `spliceCellSpan`
  ([`store.hpp:588`](apps/common/xanadu/store.hpp)); the per-op limit is that one `CompactOpNode`
  carries one span, so a multi-span cell promotes as a first span plus one splice per additional
  span (as `promote()` already does for a multi-span arena cell,
  [`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp), the promote doc comment).
  (`CompactBinaryV4` is only what lets those spliced edits round-trip on the *publish wire* — it
  carries `at`/`length` for `Splice`, [`store-slice-convergence.md`](store-slice-convergence.md)
  §U3.4 — not what lifts the per-op span count.) `XanadocLens` should emit the full span run so a
  round trip stops losing the tail of a split paragraph.

- **Presentation metadata preserved, not dropped.** `storeToSlice()` reconstructs no presentation
  metadata (§2.3). Since metadata is just cells on a rank (R13), `XanadocLens` can carry role,
  scene, and layout hints on their own attribute ranks (`d.role` already; add `d.scene`/`d.layout`
  as the lens's declared dimensions) so the DTO round-trips them instead of discarding them.

- **Provenance-stable cell identity across a re-projection.** The current id is a bare ops index
  reassigned each `sliceToStore()` (§6.2, "lossy on id"). With the `ExternalKey` recorded on a
  `d.extkey` rank (§5.3 (b)), the *same* xanadoc cell keeps the *same* external key across
  re-projections, so an edit made in one session re-targets the right cell in the next — the same
  mechanism that gives the filesystem lens inode stability.

- **Drop the paragraph-splitting heuristic where structure is explicit.** The span-overlap and
  paragraph-split heuristics exist because the DTO projector had only assembled text to work from. A
  lens reading the store's own cell structure (the `sliceToStore`/`storeToSlice` path, not the
  legacy DTO path) does not need to re-derive paragraphs by scanning text; it projects the cells the
  store already has. `XanadocLens` should be built on the convergence translators
  ([`xanadu_projector.cpp:548`](apps/common/xanadu/zigzag/xanadu_projector.cpp),
  [`:678`](apps/common/xanadu/zigzag/xanadu_projector.cpp)), retiring the heuristic DTO projector
  that step 20 of the convergence was already retiring.

### 9.3 The regression that replaces "byte-identical"

The honest check is a *meaning* round trip, which the convergence already prescribes
([`store-slice-convergence.md`](store-slice-convergence.md) §9): for each checked-in fixture,
project through `XanadocLens`, unproject, and assert `xudu-dump --section=ops` renders the same
document text and the same links — not the same op bytes. Where `XanadocLens` deliberately improves
on the old output (full spans, preserved metadata, stable ids), the fixture's *expected ops* are
regenerated in the same commit (the standing rule for a shape change, [`AGENTS.md`](AGENTS.md),
"regenerate every fixture in the same commit"), and the characterization test
(`tests/zigzag/test_xudu_convergence.cpp`) asserts the richer invariant rather than the old bytes.

______________________________________________________________________

## 9A. The Query and Logic Surface (VQL, Vlog/vprolog, VPL)

§1 claims the same VQL / VPL / Vlog runtime that works over a native slice works over a projected
source. That is a design obligation, not a freebie, and the first draft left it unspecified. This
section states what the lens must do so a query is genuinely source-agnostic, grounded in how those
runtimes reach the manifold today.

### 9A.1 VQL traverses by rank name, so canonical names are the whole game

VQL lowers a path step to a literal dimension string and resolves it through
`MultiStoreCoordinator::resolveDimension`
([`multi_store.cpp:329`](apps/common/xanadu/multi_store.cpp)), which privileges a fixed set
(`d.stores`, `d.role`, `d.clone`, …) and otherwise *mints* the named dimension
([`vql_engine.cpp:294`](apps/common/xanadu/vql/vql_engine.cpp), `traverseDimension`). There is no
enum, no alias: a query that names `d.contains` hits the `d.contains` rank or silently mints an
empty one. So §4.3.1 option (a) — every lens mints the canonical generic names — is what lets

```
##/d.contains/d.seq            -- every child of every container, in order
#cell/d.ref                    -- cross-references out of a cell
[/d.role = 'dir']              -- restrict to directory cells
```

run **identically** over a projected directory and a projected xanadoc. The prerequisites: (1)
`XanadocLens` mints `d.contains`/`d.seq`/`d.ref` (§9.2); (2) `d.mime` joins `d.role`/`d.clone` in
the privileged set ([`multi_store.cpp:333`](apps/common/xanadu/multi_store.cpp)) so an attribute
predicate resolves the same rank everywhere; (3) attribute values are read by stepping onto the
attribute cell and reading its content (`/d.role` then `value()`), which already works because the
lens attaches `d.role`/`d.mime` posward exactly as the projector does
([`xanadu_projector.cpp:37`](apps/common/xanadu/zigzag/xanadu_projector.cpp)). The DX hazard worth
documenting: an unknown generic name is **not an error** — it mints an empty dimension and the query
reads as "no data," so a lens that fails to mint a canonical name fails silently.

### 9A.2 Vlog/vprolog needs the session to project into the bound core's arena

vprolog's fact builtins enumerate the live arena: `cell_link(From,Dim,Dir,To)` and `cell_value/2`
walk `core.arena()` cell by cell with the choice-point `mark()`/`release()` as the trail
([`vortex_stdlib.cpp`](apps/common/xanadu/vortex/vortex_stdlib.cpp), `solveQueryHelper`). This is
the uniform surface the task wants — but only if the projected cells live in **the arena the bound
`VortexCore` queries**. The draft's `ProjectionSession` owns a private `ArenaManifold` (§4.4), so a
goal would enumerate nothing. The contract this section adds:

> **Project-into-bound-arena.** A `ProjectionSession` may be constructed over an existing
> `VortexCore`'s arena (or `attach`ed into it,
> [`arena_manifold.hpp`](apps/common/xanadu/zigzag/arena_manifold.hpp)), so projected cells are
> visible to `cell_link`/`cell_value` enumeration and to VQL in the same `MultiStoreCoordinator`. A
> session that owns a private arena is for a headless project→promote pipeline; a session for
> querying shares the core's arena.

With that, a Prolog goal is uniform across sources:

```prolog
cell_link(Dir, 'd.contains', pos, Child).      % children of a container
cell_link(C, 'd.role', pos, V), cell_value(V, 'dir').  % C is a directory
cell_link(A, 'd.ref', pos, B).                 % any cross-reference, any source
```

### 9A.3 Canonical derived predicates and `extkey/2`

The two-goal join `cell_link(C,'d.role',pos,V), cell_value(V,Role)` is correct but verbose, and it
binds an opaque numeric `CellRef` the querier cannot relate to the source. Two canonical additions,
written once in the shared stdlib (Vortex §9), not per lens:

- `role(C, R)` / `attribute(C, Rank, V)` — sugar over the attribute-rank join, so `role(C, 'dir')`
  is a goal rather than a hand-rolled pattern.
- `extkey(C, Key)` — expose the identity map (§5) as a fact, so a query can recover `"fs:8:1234"` or
  `"xd:…"` from a bound cell. Without it, every answer names a cell by a number meaningless outside
  the session. (When a projection is promoted, this reads the `d.extkey` rank of §5.3(b); for a live
  session it reads `ProjectionSession::keyFor`.)

### 9A.4 A test phase that proves it

The plan (§10) gains a phase that, over one `ProjectionSession` sharing a core's arena, runs the
*same* VQL query and the *same* Prolog goal against a `XanadocLens` projection and a
`FilesystemLens` projection and asserts both return structurally analogous results — the concrete
form of "traverse either as easily as a slice." Until that phase is green, §1's VQL/VPL/Vlog claim
is aspirational.

### 9A.5 What is explicitly out of scope

VPL (array language) consumes the same rank traversal as VQL, so it inherits 9A.1 with no extra
work; no VPL-specific design is needed. Mutation *through* a query (a VQL update that unprojects to
a source op) is deferred — the query surface here is read/traverse; writes go through the arena-edit
→ `commit()` path (§6), not through a query.

______________________________________________________________________

## 10. Phased Implementation Plan

Each phase is one commit; after each, `make -j$(nproc)` builds and `make -j$(nproc) test` passes.

1. **The trait and the framework, no lens.** Add `projection_lens.hpp` and
   `projection_session.{hpp,cpp}` with the project loop, the identity map, and `promoteInto()` over
   a *set* of roots (§4.4). Unit-test with a trivial in-memory lens (three nodes, two edges)
   asserting the identity map and a tight arena. No source-specific code yet.
1. **Canonical dimension names in the resolver.** Add the generic structural names and `d.mime` to
   the privileged set in `MultiStoreCoordinator::resolveDimension`
   ([`multi_store.cpp:329`](apps/common/xanadu/multi_store.cpp)) so a generic query resolves the
   same rank across sources and an unknown name is not silently minted empty (§4.3.1, §9A.1). Gate:
   a query naming a canonical generic dimension resolves without minting.
1. **`XanadocLens` on the convergence translators, minting generic names.** Re-express the xanadoc
   path behind the trait, built on `sliceToStore`/`storeToSlice` (the cell-structure path), minting
   `d.contains`/`d.seq`/`d.ref` instead of `d.doc`/`d.link` (§9.2). Gate: every fixture round-trips
   through `project → unproject` with the same document text and links under
   `xudu-dump --section=ops` (meaning, not bytes); regenerate the affected fixtures and update
   `tests/zigzag/test_xudu_convergence.cpp` to the richer invariant in the same commit.
1. **The dirty-cell + rank-level diff and topological `commit()`.** Implement the per-cell diff via
   `holdsOwn()` *and* the per-rank sequence diff for reorders (§6.4, `applyReorder`), with
   `commit()` as a topological sort over the create/reparent DAG and the per-dimension cycle policy
   (§6.5). Test with `XanadocLens`: a relink unprojects to `Store::setLink`, a content splice to
   `Store::spliceCell`, a sibling reorder to one `applyReorder`, and a browse-only session produces
   an empty dirty set (R8).
1. **The query/logic surface over a shared arena.** Wire `ProjectionSession` to project into a bound
   `VortexCore`'s arena (§9A.2) and add the canonical `role/2`, `attribute/3`, and `extkey/2`
   predicates to the shared stdlib (§9A.3). Gate: the *same* VQL query and the *same* Prolog goal
   return structurally analogous results over a `XanadocLens` and a (next phase) `FilesystemLens`
   projection (§9A.4).
1. **`FilesystemLens` projection (read-only).** The `stat`-based key, the sorted tree walk, the
   attribute ranks. Test over a fixture directory: a known tree projects to a known cell count and
   rank layout, deterministically (same tree → byte-identical arena). Re-run the §9A.4 cross-lens
   query test now that both lenses exist.
1. **`FilesystemLens` unprojection.** The `apply*()` lowerings against a temp directory: rename,
   move, chmod, utime, create, delete, `applyReorder` of a directory listing, and the refusal of a
   size change. Assert the inode key survives a rename and is rebound on a cross-device move
   (simulated).
1. **Persistent projection and `d.extkey`.** `promoteInto()` over the root set with file bytes
   imported via `importFileStore`, scalars rendered, `d.extkey` minted (with the promote-skip of
   §5.3(b)); re-open and reconcile by key. Verify `d.clone` members promote to *one* shared address
   (U-L4), not re-minted content.
1. **The vlace `openLens` entry point.** Add `openLens :: String -> IO Cell` to vlace's std:xanadu
   (§7.4), projecting into the evaluator's arena, plus the std:fun gaps the histogram needs
   (`words`, a rank-walk helper, string ordering). Gate: the word-frequency-over-a-directory program
   runs.
1. **Mixed-source session.** One `ProjectionSession` hosting a `XanadocLens` and a `FilesystemLens`
   via `ArenaManifold::attach`, proving the string-keyed map keeps the two identity spaces disjoint
   and a single query spans both.
1. **Trait v2 for structured sources (as needed).** The §8A additions (edge properties/order, node
   kind, NULL/BlobRef content, `beginBatch`/`endBatch`, multi-key rebind, `rekey`, reparent
   dimension), driven by a real structured lens (e.g. SQLite) rather than built speculatively.

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
- **U-L4. Clone dedup on promote.** `promote()` does not currently deduplicate `d.clone` subgraphs
  (§8.2 caveat), so the O(log n) sharing the design wants is a requirement on the promotion pass,
  not a property. Whether to fix it in `promote()` (resolve `cloneMaster`, reuse spans) or in each
  lens (emit content-free clone members) is unresolved; the xanadoc projector's empty-data clones
  argue for the lens, a general guarantee argues for `promote()`.
- **U-L5. The cost of `d.extkey` in hypertime.** Recording the identity key as a `d.extkey` cell
  (§5.3(b)) makes it a reachable, promotable, publishable operation — good for provenance, but it
  enlarges fixture op-sequences and turns a stale key into an authored op. The resolution (promote
  skips `d.extkey` like `d.dims`, or the key stays a side table for the publish path) is sketched,
  not decided.
- **U-L6. Durable identity for sources without one.** The inode scheme (and the no-inode fallback,
  U-L1) assumes the source offers *some* stable anchor. A structured source may not: a SQLite table
  with no primary key has only a `VACUUM`-renumbered rowid (§8A, the `rekey` bullet). The `rekey()`
  hook lets the framework rebuild the map wholesale, but *when* to invoke it — polling `stat`,
  watching for the source's own re-index event — is source-specific and unresolved.

______________________________________________________________________

## Change History

- **rev 1.** Initial design: the `ProjectionLens` trait, `ProjectionSession`, the ExternalKey ↔
  CellRef identity map, the COW-base dirty-cell diff, and the worked xanadoc + filesystem lenses.
- **rev 2.** Dropped the "zero behaviour change" constraint on the xanadoc projector (nothing is in
  production; the convergence permits a shape change). `XanadocLens` is free to improve rather than
  preserve; the regression bar became meaning-equivalence, not byte parity.
- **rev 3.** Folded six role-based critiques (technical writer, systems architect, custom-lens
  implementer, VQL user, vprolog user, vlace user). Changes: corrected the `CompactBinaryV4`
  mechanism (it carries `Splice` `at`/`length` on the wire; the span *run* model is what lifts
  single-span truncation) and the stale `(generation)` key header; made the generic dimension names
  the **canonical rank strings every lens mints** and reconciled §4.3/§7.1/§9.1 around that, since
  VQL/Vlog resolve dimensions by literal string (§4.3.1); added §9A, the query/logic surface
  (canonical-name traversal, the project-into-bound-arena contract for vprolog, `role/2`/`extkey/2`
  predicates, a cross-lens test phase); tightened the unproject half (§6.4 rank-level diff +
  `applyReorder`, §6.5 topological `commit()` with a cycle policy, multi-root `promoteInto`, the
  `d.clone` dedup and `d.extkey` promote-cost caveats); added §8A, the trait-v2 extensions a
  structured source needs (edge properties/order, node kind, NULL/BlobRef content, batch/transaction
  hooks, multi-key rebind, `rekey`, reparent dimension); added §7.4, the `openLens` entry point
  vlace lacks; and expanded the plan (§10) and open questions (§11, U-L4–U-L6) to match.
