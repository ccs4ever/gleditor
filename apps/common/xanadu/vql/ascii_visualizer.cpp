/**
 * @file ascii_visualizer.cpp
 * @brief Implementation of ASCII Art Visualizer for VQL.
 */
#include "common/xanadu/vql/ascii_visualizer.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace xanadu::vql {

using zigzag::DimVector;

namespace {

std::string formatCellBrief(const zigzag::ArenaManifold &manifold,
                            zigzag::CellRef cell) {
  if (cell == zigzag::noCell) {
    return "(null)";
  }
  std::ostringstream oss;
  oss << "#" << (cell & ~zigzag::ephemeralBit);

  zigzag::CellRef master = cell;
  for (const auto &dl : manifold.dimensionsOf(cell)) {
    std::string dname = manifold.textOf(dl.dim);
    if (dname == "d.clone" || dname == "clone") {
      master = manifold.cloneMaster(cell, dl.dim);
      break;
    }
  }

  auto kind = manifold.valueKindOf(master);
  if (kind == xanadu::ValueKind::Int64) {
    auto val = manifold.asInt64(master);
    if (val) {
      oss << ":" << *val;
    }
  } else if (kind == xanadu::ValueKind::Double) {
    auto val = manifold.asDouble(master);
    if (val) {
      oss << ":" << *val;
    }
  } else if (kind == xanadu::ValueKind::Bool) {
    auto val = manifold.asBool(master);
    if (val) {
      oss << ":" << (*val ? "true" : "false");
    }
  } else {
    std::string text = manifold.textOf(master);
    if (!text.empty()) {
      if (text.size() > 8) {
        text = text.substr(0, 7) + "~";
      }
      oss << ":\"" << text << "\"";
    }
  }
  return oss.str();
}

std::string opToString(CompOp op) {
  switch (op) {
  case CompOp::Equal:
    return "==";
  case CompOp::NotEqual:
    return "!=";
  case CompOp::LessThan:
    return "<";
  case CompOp::GreaterThan:
    return ">";
  case CompOp::LessEqual:
    return "<=";
  case CompOp::GreaterEqual:
    return ">=";
  }
  return "?";
}

} // namespace

std::string AsciiVisualizer::renderCellConnections(
    const zigzag::ArenaManifold &manifold,
    const std::vector<zigzag::CellRef> &cells,
    const std::vector<ViewDimension> &dimensions) {
  std::ostringstream oss;
  oss << "=== Xanadu Zigzag Cell Connection View ===\n";
  oss << "Viewing Dimensions: ";
  if (dimensions.empty()) {
    oss << "(none)\n";
  } else {
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
      if (i > 0) oss << "  ";
      if (i == 0) {
        oss << "[X] " << dimensions[i].name;
      } else if (i == 1) {
        oss << "[Y] " << dimensions[i].name;
      } else if (i == 2) {
        oss << "[Z] " << dimensions[i].name;
      } else {
        oss << "[" << (i + 1) << "] " << dimensions[i].name;
      }
    }
    oss << "\n";
  }

  if (cells.empty()) {
    oss << "  (empty result set)\n";
    return oss.str();
  }

  oss << "Result Set: " << cells.size() << " cell"
      << (cells.size() == 1 ? "" : "s") << "\n\n";

  // 1. 2D Spatial Lattice Projection along dimension 0 (X) and dimension 1 (Y)
  if (!dimensions.empty()) {
    zigzag::DimRef dimX       = dimensions[0].dim;
    std::string_view dimXName = dimensions[0].name;
    zigzag::DimRef dimY =
        (dimensions.size() > 1) ? dimensions[1].dim : zigzag::noCell;
    std::string_view dimYName = (dimensions.size() > 1)
                                    ? std::string_view(dimensions[1].name)
                                    : std::string_view{};
    zigzag::DimRef dimZ =
        (dimensions.size() > 2) ? dimensions[2].dim : zigzag::noCell;
    std::string_view dimZName = (dimensions.size() > 2)
                                    ? std::string_view(dimensions[2].name)
                                    : std::string_view{};

    std::unordered_set<zigzag::CellRef> resultSet(cells.begin(), cells.end());
    std::map<zigzag::CellRef, std::pair<int, int>> posMap;
    std::map<std::pair<int, int>, zigzag::CellRef> gridMap;

    int currentBaseY = 0;
    for (zigzag::CellRef startCell : cells) {
      if (posMap.contains(startCell)) continue;

      int startX = 0;
      int startY = currentBaseY;
      while (gridMap.contains({startX, startY})) {
        startY++;
      }

      std::queue<zigzag::CellRef> q;
      posMap[startCell]         = {startX, startY};
      gridMap[{startX, startY}] = startCell;
      q.push(startCell);

      int compMaxY = startY;

      while (!q.empty()) {
        zigzag::CellRef curr = q.front();
        q.pop();
        auto [cx, cy] = posMap[curr];
        if (cy > compMaxY) compMaxY = cy;

        // Explore along dimX
        if (dimX != zigzag::noCell) {
          zigzag::CellRef posTarget =
              manifold.linked(curr, dimX, zigzag::DimVector::POS);
          if (posTarget != zigzag::noCell && resultSet.contains(posTarget) &&
              !posMap.contains(posTarget)) {
            int nx = cx + 1;
            int ny = cy;
            while (gridMap.contains({nx, ny})) nx++;
            posMap[posTarget] = {nx, ny};
            gridMap[{nx, ny}] = posTarget;
            q.push(posTarget);
          }
          zigzag::CellRef negTarget =
              manifold.linked(curr, dimX, zigzag::DimVector::NEG);
          if (negTarget != zigzag::noCell && resultSet.contains(negTarget) &&
              !posMap.contains(negTarget)) {
            int nx = cx - 1;
            int ny = cy;
            while (gridMap.contains({nx, ny})) nx--;
            posMap[negTarget] = {nx, ny};
            gridMap[{nx, ny}] = negTarget;
            q.push(negTarget);
          }
        }

        // Explore along dimY
        if (dimY != zigzag::noCell) {
          zigzag::CellRef posTargetY =
              manifold.linked(curr, dimY, zigzag::DimVector::POS);
          if (posTargetY != zigzag::noCell && resultSet.contains(posTargetY) &&
              !posMap.contains(posTargetY)) {
            int nx = cx;
            int ny = cy + 1;
            while (gridMap.contains({nx, ny})) ny++;
            posMap[posTargetY] = {nx, ny};
            gridMap[{nx, ny}]  = posTargetY;
            q.push(posTargetY);
          }
          zigzag::CellRef negTargetY =
              manifold.linked(curr, dimY, zigzag::DimVector::NEG);
          if (negTargetY != zigzag::noCell && resultSet.contains(negTargetY) &&
              !posMap.contains(negTargetY)) {
            int nx = cx;
            int ny = cy - 1;
            while (gridMap.contains({nx, ny})) ny--;
            posMap[negTargetY] = {nx, ny};
            gridMap[{nx, ny}]  = negTargetY;
            q.push(negTargetY);
          }
        }
      }

      currentBaseY = compMaxY + 2;
    }

    // Determine bounds
    int minX     = 0;
    int maxX     = 0;
    int minY     = 0;
    int maxY     = 0;
    bool firstPt = true;
    for (const auto &[coord, cell] : gridMap) {
      if (firstPt) {
        minX = maxX = coord.first;
        minY = maxY = coord.second;
        firstPt     = false;
      } else {
        minX = std::min(minX, coord.first);
        maxX = std::max(maxX, coord.first);
        minY = std::min(minY, coord.second);
        maxY = std::max(maxY, coord.second);
      }
    }

    // Shift to non-negative coordinates
    int shiftX = -minX;
    int shiftY = -minY;
    if (shiftX != 0 || shiftY != 0) {
      std::map<std::pair<int, int>, zigzag::CellRef> shiftedGrid;
      for (auto &[cell, pt] : posMap) {
        pt.first += shiftX;
        pt.second += shiftY;
      }
      for (const auto &[pt, cell] : gridMap) {
        shiftedGrid[{pt.first + shiftX, pt.second + shiftY}] = cell;
      }
      gridMap = std::move(shiftedGrid);
      minX += shiftX;
      maxX += shiftX;
      minY += shiftY;
      maxY += shiftY;
    }

    oss << "[2D Spatial Lattice Projection]\n";

    constexpr int kBoxWidth      = 20;
    const std::string kBoxBorder = "+------------------+";
    const std::string kEmptyBox  = "                    ";
    const std::string kEmptyLink = "            ";

    for (int y = minY; y <= maxY; ++y) {
      bool rowHasCells = false;
      for (int x = minX; x <= maxX; ++x) {
        if (gridMap.contains({x, y})) {
          rowHasCells = true;
          break;
        }
      }
      if (!rowHasCells) continue;

      // Top border
      for (int x = minX; x <= maxX; ++x) {
        if (gridMap.contains({x, y})) {
          oss << kBoxBorder;
        } else {
          oss << kEmptyBox;
        }
        if (x < maxX) {
          oss << kEmptyLink;
        }
      }
      oss << "\n";

      // Cell content line
      for (int x = minX; x <= maxX; ++x) {
        auto it = gridMap.find({x, y});
        if (it != gridMap.end()) {
          std::string brief = formatCellBrief(manifold, it->second);
          if (brief.size() > 16) {
            brief = brief.substr(0, 15) + "~";
          }
          oss << "| " << std::left << std::setw(16) << brief << " |";
        } else {
          oss << kEmptyBox;
        }

        // Horizontal link to x + 1
        if (x < maxX) {
          auto nextIt = gridMap.find({x + 1, y});
          if (it != gridMap.end() && nextIt != gridMap.end() &&
              dimX != zigzag::noCell) {
            bool pos = (manifold.linked(it->second, dimX, DimVector::POS) ==
                        nextIt->second);
            bool neg = (manifold.linked(nextIt->second, dimX, DimVector::NEG) ==
                        it->second);
            if (pos && neg) {
              std::string lbl = std::string("+") + std::string(dimXName);
              if (lbl.size() > 5) lbl = lbl.substr(0, 5);
              std::ostringstream aoss;
              aoss << "---" << lbl << "--->";
              std::string arrow = aoss.str();
              if (arrow.size() < 12) {
                arrow.append(12 - arrow.size(), ' ');
              }
              oss << arrow;
            } else if (pos) {
              std::string lbl = std::string("+") + std::string(dimXName);
              if (lbl.size() > 5) lbl = lbl.substr(0, 5);
              std::ostringstream aoss;
              aoss << "---" << lbl << "--->";
              std::string arrow = aoss.str();
              if (arrow.size() < 12) {
                arrow.append(12 - arrow.size(), ' ');
              }
              oss << arrow;
            } else if (neg) {
              std::string lbl = std::string("-") + std::string(dimXName);
              if (lbl.size() > 5) lbl = lbl.substr(0, 5);
              std::ostringstream aoss;
              aoss << "<---" << lbl << "---";
              std::string arrow = aoss.str();
              if (arrow.size() < 12) {
                arrow.append(12 - arrow.size(), ' ');
              }
              oss << arrow;
            } else {
              oss << kEmptyLink;
            }
          } else {
            oss << kEmptyLink;
          }
        }
      }
      oss << "\n";

      // Depth Z-link line if dimZ exists
      if (dimZ != zigzag::noCell) {
        for (int x = minX; x <= maxX; ++x) {
          auto it = gridMap.find({x, y});
          if (it != gridMap.end()) {
            zigzag::CellRef zPos =
                manifold.linked(it->second, dimZ, DimVector::POS);
            zigzag::CellRef zNeg =
                manifold.linked(it->second, dimZ, DimVector::NEG);
            std::ostringstream zoss;
            if (zPos != zigzag::noCell) {
              zoss << "+" << dimZName << "->#"
                   << (zPos & ~zigzag::ephemeralBit);
            } else if (zNeg != zigzag::noCell) {
              zoss << "-" << dimZName << "<-#"
                   << (zNeg & ~zigzag::ephemeralBit);
            } else {
              zoss << dimZName << ":(nil)";
            }
            std::string zStr = zoss.str();
            if (zStr.size() > 16) {
              zStr = zStr.substr(0, 15) + "~";
            }
            oss << "| " << std::left << std::setw(16) << zStr << " |";
          } else {
            oss << kEmptyBox;
          }
          if (x < maxX) {
            oss << kEmptyLink;
          }
        }
        oss << "\n";
      }

      // Bottom border
      for (int x = minX; x <= maxX; ++x) {
        if (gridMap.contains({x, y})) {
          oss << kBoxBorder;
        } else {
          oss << kEmptyBox;
        }
        if (x < maxX) {
          oss << kEmptyLink;
        }
      }
      oss << "\n";

      // Vertical links down to y + 1
      if (y < maxY && dimY != zigzag::noCell) {
        bool hasAnyDownLink = false;
        for (int x = minX; x <= maxX; ++x) {
          auto it     = gridMap.find({x, y});
          auto downIt = gridMap.find({x, y + 1});
          if (it != gridMap.end() && downIt != gridMap.end() &&
              ((manifold.linked(it->second, dimY, DimVector::POS) ==
                downIt->second) ||
               (manifold.linked(downIt->second, dimY, DimVector::NEG) ==
                it->second))) {
            hasAnyDownLink = true;
            break;
          }
        }

        if (hasAnyDownLink) {
          // Line 1: "         |          "
          for (int x = minX; x <= maxX; ++x) {
            auto it     = gridMap.find({x, y});
            auto downIt = gridMap.find({x, y + 1});
            if (it != gridMap.end() && downIt != gridMap.end() &&
                ((manifold.linked(it->second, dimY, DimVector::POS) ==
                  downIt->second) ||
                 (manifold.linked(downIt->second, dimY, DimVector::NEG) ==
                  it->second))) {
              oss << "         |          ";
            } else {
              oss << kEmptyBox;
            }
            if (x < maxX) oss << kEmptyLink;
          }
          oss << "\n";

          // Line 2: "       +d.2         "
          for (int x = minX; x <= maxX; ++x) {
            auto it     = gridMap.find({x, y});
            auto downIt = gridMap.find({x, y + 1});
            if (it != gridMap.end() && downIt != gridMap.end() &&
                ((manifold.linked(it->second, dimY, DimVector::POS) ==
                  downIt->second) ||
                 (manifold.linked(downIt->second, dimY, DimVector::NEG) ==
                  it->second))) {
              std::string lbl = std::string("+") + std::string(dimYName);
              int leftPad     = (kBoxWidth - static_cast<int>(lbl.size())) / 2;
              std::string line(std::max(0, leftPad), ' ');
              line += lbl;
              if (line.size() < kBoxWidth) {
                line.append(kBoxWidth - line.size(), ' ');
              }
              oss << line;
            } else {
              oss << kEmptyBox;
            }
            if (x < maxX) oss << kEmptyLink;
          }
          oss << "\n";

          // Line 3: "         v          "
          for (int x = minX; x <= maxX; ++x) {
            auto it     = gridMap.find({x, y});
            auto downIt = gridMap.find({x, y + 1});
            if (it != gridMap.end() && downIt != gridMap.end() &&
                ((manifold.linked(it->second, dimY, DimVector::POS) ==
                  downIt->second) ||
                 (manifold.linked(downIt->second, dimY, DimVector::NEG) ==
                  it->second))) {
              oss << "         v          ";
            } else {
              oss << kEmptyBox;
            }
            if (x < maxX) oss << kEmptyLink;
          }
          oss << "\n";
        }
      }
    }
  }

  // 2. Link Topology Roster
  oss << "\n--- Cell Link Topology Roster ---\n";
  for (std::size_t i = 0; i < cells.size(); ++i) {
    zigzag::CellRef c = cells[i];
    oss << "Cell #" << (c & ~zigzag::ephemeralBit);
    std::string brief = formatCellBrief(manifold, c);
    oss << " [" << brief << "]:\n";

    for (const auto &vd : dimensions) {
      oss << "  " << std::left << std::setw(12) << vd.name << ": ";
      if (vd.dim == zigzag::noCell) {
        oss << "(unresolved dimension)\n";
        continue;
      }
      zigzag::CellRef neg = manifold.linked(c, vd.dim, zigzag::DimVector::NEG);
      zigzag::CellRef pos = manifold.linked(c, vd.dim, zigzag::DimVector::POS);

      // Negward (-)
      oss << "(-) ";
      if (neg != zigzag::noCell) {
        oss << "<- " << std::left << std::setw(16)
            << formatCellBrief(manifold, neg);
      } else {
        oss << std::left << std::setw(19) << "(nil)";
      }

      // Posward (+)
      oss << " (+) ";
      if (pos != zigzag::noCell) {
        oss << "-> " << formatCellBrief(manifold, pos);
      } else {
        oss << "(nil)";
      }
      oss << "\n";
    }
    if (i + 1 < cells.size()) {
      oss << "\n";
    }
  }

  return oss.str();
}

std::string
AsciiVisualizer::renderRankGrid(const zigzag::ArenaManifold &manifold,
                                zigzag::CellRef startCell, zigzag::DimRef dimX,
                                zigzag::DimRef dimY, std::string_view dimXName,
                                std::string_view dimYName, int radius) {
  if (!manifold.contains(startCell) && startCell != zigzag::noCell) {
    return "(start cell not found in manifold)\n";
  }

  // Build coordinate map (x, y) -> CellRef
  std::map<std::pair<int, int>, zigzag::CellRef> grid;
  grid[{0, 0}] = startCell;

  // Trace along Y axis from origin
  for (int y = -radius; y <= radius; ++y) {
    if (y == 0) {
      continue;
    }
    // Walk from origin
    zigzag::CellRef curr  = startCell;
    int steps             = std::abs(y);
    zigzag::DimVector dir = zigzag::fromSign(y);
    for (int s = 0; s < steps && curr != zigzag::noCell; ++s) {
      curr = manifold.linked(curr, dimY, dir);
    }
    grid[{0, y}] = curr;
  }

  // Trace along X axis for each row
  for (int y = -radius; y <= radius; ++y) {
    zigzag::CellRef rowOrigin = grid[{0, y}];
    if (rowOrigin == zigzag::noCell) {
      continue;
    }
    for (int x = -radius; x <= radius; ++x) {
      if (x == 0) {
        continue;
      }
      zigzag::CellRef curr  = rowOrigin;
      int steps             = std::abs(x);
      zigzag::DimVector dir = zigzag::fromSign(x);
      for (int s = 0; s < steps && curr != zigzag::noCell; ++s) {
        curr = manifold.linked(curr, dimX, dir);
      }
      grid[{x, y}] = curr;
    }
  }

  std::ostringstream oss;
  oss << "=== 2D Rank Grid (dimX: " << dimXName << " ->, dimY: " << dimYName
      << " v) ===\n\n";

  for (int y = -radius; y <= radius; ++y) {
    // Top border of row
    for (int x = -radius; x <= radius; ++x) {
      auto it = grid.find({x, y});
      if (it != grid.end() && it->second != zigzag::noCell) {
        oss << "+--------------+";
      } else {
        oss << "                ";
      }
      if (x < radius) {
        oss << "  ";
      }
    }
    oss << "\n";

    // Cell content of row
    for (int x = -radius; x <= radius; ++x) {
      auto it = grid.find({x, y});
      if (it != grid.end() && it->second != zigzag::noCell) {
        std::string brief = formatCellBrief(manifold, it->second);
        if (brief.size() > 12) {
          brief = brief.substr(0, 11) + "~";
        }
        oss << "| " << std::left << std::setw(12) << brief << " |";
      } else {
        oss << "                ";
      }
      // Horizontal link indicator
      if (x < radius) {
        auto nextIt = grid.find({x + 1, y});
        if (it != grid.end() && it->second != zigzag::noCell &&
            nextIt != grid.end() && nextIt->second != zigzag::noCell) {
          oss << "->";
        } else {
          oss << "  ";
        }
      }
    }
    oss << "\n";

    // Bottom border of row
    for (int x = -radius; x <= radius; ++x) {
      auto it = grid.find({x, y});
      if (it != grid.end() && it->second != zigzag::noCell) {
        oss << "+--------------+";
      } else {
        oss << "                ";
      }
      if (x < radius) {
        oss << "  ";
      }
    }
    oss << "\n";

    // Vertical links down to next row
    if (y < radius) {
      for (int x = -radius; x <= radius; ++x) {
        auto it     = grid.find({x, y});
        auto downIt = grid.find({x, y + 1});
        if (it != grid.end() && it->second != zigzag::noCell &&
            downIt != grid.end() && downIt->second != zigzag::noCell) {
          oss << "       |        ";
        } else {
          oss << "                ";
        }
        if (x < radius) {
          oss << "  ";
        }
      }
      oss << "\n";
    }
  }

  return oss.str();
}

std::string
AsciiVisualizer::renderCellInspection(const zigzag::ArenaManifold &manifold,
                                      zigzag::CellRef cell) {
  std::ostringstream oss;
  oss << "--------------------------------------------------\n";
  oss << "Cell: #" << (cell & ~zigzag::ephemeralBit);
  if (cell & zigzag::ephemeralBit) {
    oss << " (ephemeral)";
  }
  oss << " [0x" << std::hex << cell << std::dec << "]\n";

  auto kind = manifold.valueKindOf(cell);
  oss << "ValueKind: ";
  switch (kind) {
  case xanadu::ValueKind::None:
    oss << "None (string/untyped)\n";
    break;
  case xanadu::ValueKind::Double:
    oss << "Double (" << *manifold.asDouble(cell) << ")\n";
    break;
  case xanadu::ValueKind::Int64:
    oss << "Int64 (" << *manifold.asInt64(cell) << ")\n";
    break;
  case xanadu::ValueKind::Bool:
    oss << "Bool (" << (*manifold.asBool(cell) ? "true" : "false") << ")\n";
    break;
  }

  std::string text = manifold.textOf(cell);
  oss << "Text Content: \"" << text << "\" (" << text.size() << " bytes)\n";

  auto dims = manifold.dimensionsOf(cell);
  oss << "Dimensions linked (" << dims.size() << "):\n";
  for (const auto &dl : dims) {
    oss << "  dim #" << (dl.dim & ~zigzag::ephemeralBit) << ": ";
    if (dl.neg != zigzag::noCell) {
      oss << "negward -> #" << (dl.neg & ~zigzag::ephemeralBit) << "  ";
    } else {
      oss << "negward -> (nil)  ";
    }
    if (dl.pos != zigzag::noCell) {
      oss << "posward -> #" << (dl.pos & ~zigzag::ephemeralBit) << "\n";
    } else {
      oss << "posward -> (nil)\n";
    }
  }
  oss << "--------------------------------------------------\n";
  return oss.str();
}

std::string AsciiVisualizer::renderQueryResults(
    const zigzag::ArenaManifold &manifold,
    const std::vector<zigzag::CellRef> &results) {
  std::ostringstream oss;
  oss << "=== VQL Query Results (" << results.size() << " cells) ===\n";
  if (results.empty()) {
    oss << "  (empty result set)\n";
    return oss.str();
  }

  for (std::size_t i = 0; i < results.size(); ++i) {
    oss << "  [" << i << "] " << formatCellBrief(manifold, results[i]) << "\n";
  }
  return oss.str();
}

static void dumpPathExpression(std::ostringstream &oss,
                               const PathExpression &path,
                               const std::string &prefix) {
  oss << prefix << "+-- Anchor: ";
  switch (path.anchor.kind) {
  case AnchorKind::Home:
    oss << "##";
    break;
  case AnchorKind::NamedStore:
    oss << "##" << path.anchor.name;
    break;
  case AnchorKind::Root:
    oss << "#";
    break;
  case AnchorKind::Cursor:
    oss << "^";
    break;
  case AnchorKind::NamedCursor:
    oss << "^" << path.anchor.name;
    break;
  case AnchorKind::Variable:
    oss << "$" << path.anchor.name;
    break;
  case AnchorKind::LiteralCellId:
    oss << path.anchor.cellId;
    break;
  case AnchorKind::Context:
    oss << ".";
    break;
  case AnchorKind::Create:
    oss << "%";
    if (path.anchor.createValue && !path.anchor.createValue->literal.empty()) {
      oss << path.anchor.createValue->literal;
    }
    break;
  }
  if (path.anchor.derefMaster) {
    oss << ">";
  }
  oss << "\n";

  for (std::size_t i = 0; i < path.steps.size(); ++i) {
    const auto &step = path.steps[i];
    oss << prefix << "|   +-- Step [" << i << "]: ";
    if (std::holds_alternative<SignedDimensionStep>(step.selector)) {
      const auto &s = std::get<SignedDimensionStep>(step.selector);
      oss << (zigzag::isNegward(s.direction) ? "-" : "") << s.dimName;
      if (s.placement == Placement::From) {
        oss << "::from";
      } else if (s.placement == Placement::Rank) {
        oss << "::rank";
      } else if (s.placement == Placement::Head) {
        oss << "::head";
      } else if (s.placement == Placement::Tail) {
        oss << "::tail";
      }
      for (const auto &c : s.creates) {
        oss << "%";
        if (c.kind == CreateValue::Kind::Literal) {
          oss << "\"" << c.literal << "\"";
        }
      }
    } else if (std::holds_alternative<FunctionInvocation>(step.selector)) {
      const auto &fn = std::get<FunctionInvocation>(step.selector);
      oss << fn.name << "(...)";
    }
    if (step.derefMaster) {
      oss << ">";
    }
    if (step.yieldMode == YieldMode::New) {
      oss << "!new";
    } else if (step.yieldMode == YieldMode::Last) {
      oss << "!last";
    } else if (step.yieldMode == YieldMode::Both) {
      oss << "!both";
    } else if (step.yieldMode == YieldMode::Keep) {
      oss << "!keep";
    }
    oss << "\n";

    if (step.rangeClamp.has_value()) {
      oss << prefix << "|       RangeClamp: [" << step.rangeClamp->start;
      if (step.rangeClamp->end.has_value()) {
        oss << ", " << *step.rangeClamp->end;
      }
      oss << "]\n";
    }

    if (!step.predicates.empty()) {
      oss << prefix << "|       Predicates (" << step.predicates.size()
          << "):\n";
      for (const auto &pred : step.predicates) {
        for (const auto &term : pred.terms) {
          for (const auto &factor : term.factors) {
            oss << prefix << "|         +-- " << (factor.negated ? "not " : "");
            if (std::holds_alternative<ComparisonExpr>(factor.test)) {
              const auto &cmp = std::get<ComparisonExpr>(factor.test);
              oss << "Comparison (" << opToString(cmp.op) << ")\n";
            } else {
              oss << "PredicateTest\n";
            }
          }
        }
      }
    }
  }

  if (path.cloneTail.has_value()) {
    oss << prefix << "+-- CloneTail: >< (" << path.cloneTail->operands.size()
        << ")\n";
  }
}

std::string AsciiVisualizer::renderAST(const PathExpression &path) {
  std::ostringstream oss;
  oss << "PathExpression:\n";
  dumpPathExpression(oss, path, "  ");
  return oss.str();
}

std::string AsciiVisualizer::renderAST(const QueryExpression &query) {
  std::ostringstream oss;
  if (std::holds_alternative<PathExpression>(query.expr)) {
    return renderAST(std::get<PathExpression>(query.expr));
  }

  const auto &block = std::get<ExecutionBlock>(query.expr);
  oss << "ExecutionBlock:\n";
  oss << "  +-- Bindings (" << block.bindings.size() << "):\n";
  for (const auto &b : block.bindings) {
    if (std::holds_alternative<ForClause>(b)) {
      const auto &fc = std::get<ForClause>(b);
      oss << "  |   +-- For: $" << fc.varName << " in\n";
      dumpPathExpression(oss, fc.inPath, "  |       ");
    } else {
      const auto &lc = std::get<LetClause>(b);
      oss << "  |   +-- Let: $" << lc.varName << " :=\n";
    }
  }

  if (block.where.has_value()) {
    oss << "  +-- Where Clause (terms: " << block.where->condition.terms.size()
        << ")\n";
  }

  oss << "  +-- Action Clause: ";
  if (std::holds_alternative<ReturnClause>(block.action.clause)) {
    oss << "Return\n";
  } else if (std::holds_alternative<EffectClause>(block.action.clause)) {
    oss << "Weave/Effect\n";
  } else if (std::holds_alternative<ConditionalClause>(block.action.clause)) {
    oss << "Conditional (If/Else)\n";
  }

  return oss.str();
}

} // namespace xanadu::vql
