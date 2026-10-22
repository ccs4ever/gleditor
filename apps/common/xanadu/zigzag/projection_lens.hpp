/**
 * @file projection_lens.hpp
 * @brief The source-agnostic ProjectionLens trait and its value types
 *        (design/anything-to-zigzag-lens.md §4).
 *
 * A ProjectionLens turns some external structure (a xanadoc store, a directory
 * tree, ...) into ZigZag cells, and (optionally) unprojects edits back to that
 * structure's own operations. The project half is enumerative: a lens declares
 * the dimensions it needs, then streams its nodes and edges in ExternalKey
 * space; the framework (ProjectionSession) mints a cell per node, links the
 * edges, and records the ExternalKey <-> CellRef identity map. The unproject
 * half (apply*) is driven by the session's dirty-cell diff.
 *
 * Dimension names are the load-bearing contract (§4.3.1): VQL/Vlog resolve a
 * dimension by its literal rank-name string, so a lens mints the CANONICAL
 * generic names (d.contains/d.seq/d.ref/d.clone) wherever a concept has a
 * generic analogue, and only its own private names where it does not. That is
 * what lets one query traverse any projected source.
 */
#ifndef ZIGZAG_PROJECTION_LENS_HPP
#define ZIGZAG_PROJECTION_LENS_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag {

/// A source node's own stable, lens-defined identity, as a canonical opaque
/// string. A xanadoc encodes "xd:<documentId>:<version>:<span>"; a filesystem
/// encodes "fs:<dev>:<ino>" (or "fsp:<hash>"). Only the lens computes one.
using ExternalKey = std::string;

/// The canonical structural dimensions every lens maps a concept onto when a
/// generic analogue exists. The STRING a lens mints is in GenericDimName; the
/// enum is the C++-side token (§4.3).
enum class GenericDim : std::uint8_t {
  Sequence,    ///< d.seq      -- ordered siblings / reading flow
  Containment, ///< d.contains -- parent -> child
  Reference,   ///< d.ref      -- a cross-edge (xanalink, symlink)
  Clone,       ///< d.clone    -- shared-content identity
};

/// The canonical rank-name string for a GenericDim -- what lands in the
/// manifold and what a query names.
[[nodiscard]] inline std::string_view genericDimName(GenericDim d) noexcept {
  switch (d) {
  case GenericDim::Sequence:
    return "d.seq";
  case GenericDim::Containment:
    return "d.contains";
  case GenericDim::Reference:
    return "d.ref";
  case GenericDim::Clone:
    return "d.clone";
  }
  return "d.seq";
}

/// A node's content before it is minted: text, a scalar, or empty.
using NodeContent =
    std::variant<std::monostate, std::string, double, bool, std::int64_t>;

/// One projected node. The lens fills this; the framework mints a cell and
/// records key <-> CellRef. @p attributes name attribute ranks (e.g.
/// {"d.role","dir"}) attached posward of the cell, exactly as the projector's
/// d.role/d.mime pattern does.
struct SourceNode {
  ExternalKey key;
  NodeContent content;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::uint64_t order{0}; ///< total order within siblings (determinism)
};

/// One edge the lens asserts, in ExternalKey space, so the framework resolves
/// both ends through the identity map without the lens touching a CellRef.
struct SourceEdge {
  ExternalKey from;
  ExternalKey to;
  std::string dimension; ///< canonical rank name, e.g. "d.contains"
  DimVector dir{DimVector::POS};
};

/// A bidirectional translator between some external structure and a ZigZag
/// manifold. The project half is required; the unproject half has default
/// implementations that refuse, so a read-only lens overrides nothing.
class ProjectionLens {
public:
  virtual ~ProjectionLens() = default;

  // -- source -> cells -------------------------------------------------------

  /// Dimensions this lens mints, in order, each (canonical name, GenericDim or
  /// nullopt-for-custom). Declared up front so the framework mints them before
  /// any node references one.
  [[nodiscard]] virtual std::vector<
      std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const = 0;

  /// Enumerate every node in a total order. Pull-based so a huge source
  /// streams.
  virtual void forEachNode(
      gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const = 0;

  /// Enumerate every edge in ExternalKey space, after all nodes are minted.
  virtual void forEachEdge(
      gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const = 0;

  // -- refresh-time hooks (opt-in; defaults keep one-shot lenses working) ----

  /// Attributes to stamp on a cell whose key was absent from the latest
  /// refresh() enumeration -- stale-marking is the lens's vocabulary, not the
  /// framework's. Default: absent keys stay untouched.
  [[nodiscard]] virtual std::vector<std::pair<std::string, std::string>>
  staleAttributes(const ExternalKey & /*key*/) const {
    return {};
  }

  /// Lens-declared roots. Emit every key that should appear in
  /// ProjectionSession::roots() and return true to override the default
  /// structural rule (a node that is no posward structural edge's target is
  /// a root). Default returns false -> structural election. Lenses whose
  /// subjects float outside the containment tree (SqlLens's shared row
  /// subjects) declare their roots rather than flooding roots() with every
  /// node ever seen.
  virtual bool
  forEachRoot(gleditor::cpp26::function_ref<void(const ExternalKey &)>
              /*emit*/) const {
    return false;
  }

  // -- cells -> source (unproject); default = refuse -------------------------

  /// Apply a content change back to the source. Returns the (possibly
  /// relocated) ExternalKey, or an error. Default: refuse (read-only lens).
  [[nodiscard]] virtual std::expected<ExternalKey, std::string>
  applyContentChange(const ExternalKey & /*node*/,
                     std::string_view /*newContent*/) {
    return std::unexpected("this lens is read-only");
  }

  /// Apply an attribute-rank change. Default: refuse.
  [[nodiscard]] virtual std::expected<ExternalKey, std::string>
  applyAttributeChange(const ExternalKey & /*node*/, std::string_view /*rank*/,
                       std::string_view /*newValue*/) {
    return std::unexpected("this lens is read-only");
  }
};

/// A ProjectionLens over an unbounded source that materializes bounded pages.
/// Orthogonal to the projection trait -- implement alongside it (SqlLens is
/// the first); the session reaches discardMaterializedAfter through a
/// dynamic_cast so it never needs the concrete type.
class WindowedLens {
public:
  virtual ~WindowedLens() = default;

  /// Materialize the window @p windowKey names -- the fetch behind a deferred
  /// stub. Driven by the app (it keeps a non-owning pointer to the concrete
  /// lens), not the session. Default refuses: a lens may be windowed only in
  /// name.
  [[nodiscard]] virtual std::expected<void, std::string>
  materializePage(const ExternalKey & /*windowKey*/) {
    return std::unexpected("no window materialization");
  }

  /// Drop every buffered window whose materialization epoch follows
  /// @p boundaryKey's -- the lens-side half of ProjectionSession::rewind.
  /// The page buffer is off-arena state the arena's release() cannot touch;
  /// without this the next refresh would re-emit buffered rows and resurrect
  /// the tossed window. Default no-op for lenses with no buffer.
  virtual void discardMaterializedAfter(const ExternalKey & /*boundaryKey*/) {}
};

} // namespace zigzag

#endif // ZIGZAG_PROJECTION_LENS_HPP
