#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag {

/// Why a result cell cannot be carried across a release() exactly.
enum class CopyOutRefusal : std::uint8_t {
  ProxyCell,  ///< names a cell in a foreign space the release detaches
  QuotedCell, ///< quotes spans in a space the release detaches, or is an
              ///< occurrence view
  LinkedCell, ///< links to a cell that outlives the release, or on a dimension
              ///< the release un-names
  DimensionCell, ///< is an arena dimension, which release() un-names
  HandleValue,   ///< its value is a handle into the program being released
  RuntimeObject, ///< claims to be a runtime object (a closure or bindings
                 ///< record), which names cells by integer reference
};

[[nodiscard]] constexpr const char *toString(CopyOutRefusal why) noexcept {
  switch (why) {
  case CopyOutRefusal::ProxyCell:
    return "proxy cell";
  case CopyOutRefusal::QuotedCell:
    return "quoted cell in a space the program attached";
  case CopyOutRefusal::LinkedCell:
    return "cell linked to a cell that outlives the program";
  case CopyOutRefusal::DimensionCell:
    return "dimension cell";
  case CopyOutRefusal::HandleValue:
    return "handle-valued cell";
  case CopyOutRefusal::RuntimeObject:
    return "runtime object, which names program cells by reference";
  }
  return "unknown";
}

struct CopyOutRefused {
  CopyOutRefusal reason{CopyOutRefusal::LinkedCell};
  CellRef cell{noCell};
};

[[nodiscard]] std::string toString(const CopyOutRefused &refused);

/**
 * @brief Whether @p cell is, or is an integer naming, a runtime object: the
 *        `<runtime-object>` root of a closure or bindings record, or a cell
 *        with a link on the runtime-object claim dimension.
 *
 * Such an object holds cell references as integers, so copying its cells
 * (even exactly) leaves those integers pointing at cells that are gone or
 * different. A plain integer that happens to name one is refused too.
 */
[[nodiscard]] bool
carriesRuntimeObjectClaim(const ArenaManifold &arena, CellRef cell,
                          std::optional<DimRef> claimDimension);

/// The runtime object @p cell is or names (an integer naming one), if any.
[[nodiscard]] std::optional<CellRef>
runtimeObjectOf(const ArenaManifold &arena, CellRef cell,
                std::optional<DimRef> claimDimension);

/**
 * @brief Cells a program minted under a mark, read out so they can outlive
 *        that mark's release().
 *
 * release() truncates everything above the mark, so a cell cannot be moved
 * below it: it is described here, the mark is released, and equal cells are
 * minted afresh afterwards. A cell is described by its value, its content run
 * (a quote keeps its space and spans) and its links to other cells the program
 * minted, and the cells those links reach are carried with it, so a linked
 * result keeps its whole topology. Anything else is refused by name rather than
 * copied inexactly. A cell that is not above the mark is not the program's,
 * and is passed through unchanged.
 */
class CopyOut {
public:
  /// Describe the cells of @p cells that @p arena minted since @p floor was
  /// taken. One description per distinct cell, so a cell appearing twice
  /// stays one cell.
  ///
  /// A cell that merely names an older runtime object (an integer holding its
  /// ref) is copied as it is when @p mayShareRuntimeObject agrees that nothing
  /// the object names is the program's; every other runtime object is refused.
  [[nodiscard]] static std::expected<CopyOut, CopyOutRefused>
  capture(const ArenaManifold &arena, const Mark &floor,
          std::span<const CellRef> cells,
          std::optional<DimRef> runtimeObjectClaim,
          gleditor::cpp26::function_ref<bool(CellRef)> mayShareRuntimeObject);

  /// Mint the described cells in @p arena, which has been released to the
  /// floor, and answer the replacement of each cell captured.
  [[nodiscard]] std::unordered_map<CellRef, CellRef>
  materialize(ArenaManifold &arena) const;

  [[nodiscard]] std::size_t size() const noexcept { return cells_.size(); }

private:
  /// One run piece: a span that outlives the release, or scratch text to be
  /// interned again.
  struct Piece {
    xanadu::PrimediaSpan span;
    std::string scratch;
    bool isScratch{false};
  };
  /// One link, to the cell it reached as the program held it.
  struct Edge {
    DimRef dim{noCell};
    DimVector dir{DimVector::POS};
    CellRef target{noCell};
  };
  struct Described {
    CellRef source{noCell};
    std::uint8_t valueKind{0};
    std::uint64_t valueBits{0};
    std::vector<Piece> run;
    /// Set for a cell quoting a store's spans, which stay addressed there.
    std::optional<std::uint32_t> quoteSpace;
    std::vector<Edge> edges;
  };
  std::vector<Described> cells_;
};

/// Rewrite every ref of @p cells that @p replacements names.
void applyCopyOut(const std::unordered_map<CellRef, CellRef> &replacements,
                  std::vector<CellRef> &cells) noexcept;

} // namespace zigzag
