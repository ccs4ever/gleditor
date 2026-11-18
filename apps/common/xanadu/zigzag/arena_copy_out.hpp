#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag {

/// Why a result cell cannot be carried across a release() exactly.
enum class CopyOutRefusal : std::uint8_t {
  ProxyCell,     ///< names a cell in a foreign space the release detaches
  QuotedCell,    ///< quotes spans addressed in a space or occurrence view
  LinkedCell,    ///< has topology; a detached topology copy is not built
  DimensionCell, ///< is an arena dimension, which release() un-names
  HandleValue,   ///< its value is a handle into the program being released
};

[[nodiscard]] constexpr const char *toString(CopyOutRefusal why) noexcept {
  switch (why) {
  case CopyOutRefusal::ProxyCell:
    return "proxy cell";
  case CopyOutRefusal::QuotedCell:
    return "quoted cell";
  case CopyOutRefusal::LinkedCell:
    return "cell with links";
  case CopyOutRefusal::DimensionCell:
    return "dimension cell";
  case CopyOutRefusal::HandleValue:
    return "handle-valued cell";
  }
  return "unknown";
}

struct CopyOutRefused {
  CopyOutRefusal reason{CopyOutRefusal::LinkedCell};
  CellRef cell{noCell};
};

[[nodiscard]] std::string toString(const CopyOutRefused &refused);

/**
 * @brief Cells a program minted under a mark, read out so they can outlive
 *        that mark's release().
 *
 * release() truncates everything above the mark, so a cell cannot be moved
 * below it: it is described here, the mark is released, and equal cells are
 * minted afresh afterwards. Only cells whose whole identity is a value and a
 * content run are described -- anything else is refused by name rather than
 * copied inexactly. A cell that is not above the mark is not the program's,
 * and is passed through unchanged.
 */
class CopyOut {
public:
  /// Describe the cells of @p cells that @p arena minted since @p floor was
  /// taken. One description per distinct cell, so a cell appearing twice
  /// stays one cell.
  [[nodiscard]] static std::expected<CopyOut, CopyOutRefused>
  capture(const ArenaManifold &arena, const Mark &floor,
          std::span<const CellRef> cells);

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
  struct Described {
    CellRef source{noCell};
    std::uint8_t valueKind{0};
    std::uint64_t valueBits{0};
    std::vector<Piece> run;
  };
  std::vector<Described> cells_;
};

/// Rewrite every ref of @p cells that @p replacements names.
void applyCopyOut(const std::unordered_map<CellRef, CellRef> &replacements,
                  std::vector<CellRef> &cells) noexcept;

} // namespace zigzag
