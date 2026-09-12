/**
 * @file ascii_visualizer.hpp
 * @brief ASCII Art Visualizer for VQL query debugging, AST dumping, and 2D
 * Zigzag rank grid rendering.
 */
#ifndef COMMON_XANADU_VQL_ASCII_VISUALIZER_HPP
#define COMMON_XANADU_VQL_ASCII_VISUALIZER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

/**
 * @struct ViewDimension
 * @brief Active dimension name and reference for multi-dimensional cell
 * projection.
 */
struct ViewDimension {
  std::string name;
  zigzag::DimRef dim{zigzag::noCell};
};

class AsciiVisualizer {
public:
  /// Renders a visible cell connection view (2D spatial lattice projection and
  /// link topology roster) across user-selected viewing dimensions.
  static std::string
  renderCellConnections(const zigzag::ArenaManifold &manifold,
                        const std::vector<zigzag::CellRef> &cells,
                        const std::vector<ViewDimension> &dimensions);

  /// Renders a 2D rank grid around startCell along dimX (horizontal) and dimY
  /// (vertical). Radius determines how many steps posward/negward to trace.
  static std::string renderRankGrid(const zigzag::ArenaManifold &manifold,
                                    zigzag::CellRef startCell,
                                    zigzag::DimRef dimX, zigzag::DimRef dimY,
                                    std::string_view dimXName = "dx",
                                    std::string_view dimYName = "dy",
                                    int radius                = 2);

  /// Renders a detailed text/ASCII inspection card for a single cell (id, kind,
  /// content, links).
  static std::string renderCellInspection(const zigzag::ArenaManifold &manifold,
                                          zigzag::CellRef cell);

  /// Renders a sequence of query result cells.
  static std::string
  renderQueryResults(const zigzag::ArenaManifold &manifold,
                     const std::vector<zigzag::CellRef> &results);

  /// Renders an ASCII tree representation of an AST.
  static std::string renderAST(const QueryExpression &query);
  static std::string renderAST(const PathExpression &path);
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_ASCII_VISUALIZER_HPP
